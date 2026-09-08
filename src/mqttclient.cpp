// mqttclient.cpp
// 极简 MQTT 3.1.1 发布端客户端实现。
// 协议参考：MQTT Version 3.1.1 (OASIS Standard)。
// 所有控制包 = 固定头(1 字节类型/标志 + 1~4 字节剩余长度 varint) + 变长头 + 载荷。

#include "mqttclient.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QVector>

namespace {

constexpr int kReconnectIntervalMs = 5 * 1000;

// CONNACK 返回码（MQTT 3.1.1 表 3.1）
enum ConnackCode {
    CONNACK_ACCEPTED            = 0,
    CONNACK_BAD_PROTOCOL        = 1,
    CONNACK_CLIENT_REJECTED     = 2,
    CONNACK_SERVER_UNAVAILABLE  = 3,
    CONNACK_BAD_CREDENTIALS     = 4,
    CONNACK_NOT_AUTHORIZED      = 5
};

} // namespace

MqttClient::MqttClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_port(1883)
    , m_keepAliveSec(60)
    , m_autoReconnect(true)
    , m_brokerConnected(false)
    , m_userRequestedDisconnect(false)
    , m_droppedCount(0)
    , m_dropWarningEmitted(false)
    , m_publishQos(0)
    , m_nextPacketId(1)
{
    connect(m_socket, &QTcpSocket::connected, this, &MqttClient::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &MqttClient::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &MqttClient::onSocketReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &MqttClient::onSocketError);

    m_pingTimer.setInterval(m_keepAliveSec * 1000 / 2);
    connect(&m_pingTimer, &QTimer::timeout, this, &MqttClient::onPingTimer);

    m_reconnectTimer.setInterval(kReconnectIntervalMs);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &MqttClient::onReconnectTimer);

    m_retransmitTimer.setInterval(2 * 1000);   // QoS1：2 秒无 PUBACK 即 DUP 重发
    connect(&m_retransmitTimer, &QTimer::timeout, this, &MqttClient::sendPendingRetransmits);
}

MqttClient::~MqttClient()
{
    m_userRequestedDisconnect = true;
    if (m_brokerConnected)
        sendDisconnect();
}

void MqttClient::setBroker(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
}

void MqttClient::setCredentials(const QString &clientId,
                                const QString &username, const QString &password)
{
    m_clientId = clientId;
    m_username = username;
    m_password = password;
}

void MqttClient::setKeepAlive(int seconds)
{
    m_keepAliveSec = seconds > 0 ? seconds : 60;
    m_pingTimer.setInterval(m_keepAliveSec * 1000 / 2);
}

void MqttClient::setAutoReconnect(bool enabled)
{
    m_autoReconnect = enabled;
}

void MqttClient::setPublishQos(int qos)
{
    m_publishQos = (qos >= 1) ? 1 : 0;
}

bool MqttClient::isConnectedToBroker() const
{
    return m_brokerConnected;
}

QString MqttClient::brokerInfo() const
{
    return QStringLiteral("%1:%2").arg(m_host.isEmpty() ? QStringLiteral("--") : m_host).arg(m_port);
}

// ---------------- 连接管理 ----------------

void MqttClient::connectToBroker()
{
    if (m_host.isEmpty()) {
        emit errorOccurred(tr("MQTT broker address is not configured"));
        return;
    }
    if (m_socket->state() == QAbstractSocket::ConnectingState) {
        // 上一次连接尝试还没结果（如 broker 未就绪时的挂起连接）：
        // 中止它，立即用当前配置重连，避免用户再点"保存并连接"没有反应
        m_socket->abort();
    } else if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        return;                                     // 已连接：忽略重复请求
    }

    m_userRequestedDisconnect = false;
    m_brokerConnected = false;
    m_buffer.clear();
    m_socket->connectToHost(m_host, m_port);
}

void MqttClient::disconnectFromBroker()
{
    m_userRequestedDisconnect = true;
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    if (m_brokerConnected)
        sendDisconnect();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->disconnectFromHost();
}

void MqttClient::scheduleReconnect()
{
    if (!m_autoReconnect || m_userRequestedDisconnect)
        return;
    if (!m_reconnectTimer.isActive())
        m_reconnectTimer.start();
}

void MqttClient::onSocketConnected()
{
    sendConnect();
}

void MqttClient::onSocketDisconnected()
{
    const bool wasConnected = m_brokerConnected;
    resetSessionState();
    if (wasConnected)
        emit disconnected();
    scheduleReconnect();
}

void MqttClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    if (m_socket->error() != QAbstractSocket::RemoteHostClosedError)
        emit errorOccurred(tr("MQTT connection error: %1").arg(m_socket->errorString()));

    // 连接阶段失败（broker 未启动/地址不可达/端口错误等）：TCP 从未连上，
    // 不会触发 disconnected()，这里兜底启动自动重连循环——否则状态会永远
    // 卡在"连接中"，之后再启动 broker 也不会重试。
    // 注意：errorOccurred 发出时 socket 状态往往还是 ConnectingState（尚未切到
    // Unconnected），因此这里不检查状态；重连定时器触发时自带状态校验，
    // 且 scheduleReconnect 对"定时器已在跑"幂等。认证被拒等不可恢复错误已在
    // handleConnack 里置 m_userRequestedDisconnect，不会进入重试。
    if (!m_userRequestedDisconnect && !m_brokerConnected)
        scheduleReconnect();
}

void MqttClient::onReconnectTimer()
{
    if (m_userRequestedDisconnect || m_socket->state() != QAbstractSocket::UnconnectedState)
        return;
    emit errorOccurred(tr("MQTT reconnecting to %1 ...").arg(brokerInfo()));
    m_socket->connectToHost(m_host, m_port);
}

void MqttClient::onPingTimer()
{
    if (m_brokerConnected)
        sendPingreq();
}

void MqttClient::resetSessionState()
{
    m_brokerConnected = false;
    m_pingTimer.stop();
    m_buffer.clear();
    m_pending.clear();              // 断线即清空 QoS1 待确认队列（会话已失效）
    m_retransmitTimer.stop();
    m_dropWarningEmitted = false;   // 新的断连周期允许再次告警
}

// ---------------- 编码辅助 ----------------

QByteArray MqttClient::encodeRemainingLength(int length)
{
    // MQTT 3.1.1 剩余长度 varint：每字节低 7 位有效，最高位为延续标志
    QByteArray out;
    do {
        char digit = static_cast<char>(length % 128);
        length /= 128;
        if (length > 0)
            digit |= static_cast<char>(0x80);
        out.append(digit);
    } while (length > 0);
    return out;
}

QByteArray MqttClient::encodeString(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    QByteArray out;
    out.append(static_cast<char>((utf8.size() >> 8) & 0xFF));
    out.append(static_cast<char>(utf8.size() & 0xFF));
    out.append(utf8);
    return out;
}

QString MqttClient::connackCodeText(int code)
{
    switch (code) {
    case CONNACK_ACCEPTED:           return tr("Connection accepted");
    case CONNACK_BAD_PROTOCOL:       return tr("Unsupported protocol version");
    case CONNACK_CLIENT_REJECTED:    return tr("Client identifier rejected");
    case CONNACK_SERVER_UNAVAILABLE: return tr("Server unavailable");
    case CONNACK_BAD_CREDENTIALS:    return tr("Bad username or password");
    case CONNACK_NOT_AUTHORIZED:     return tr("Not authorized");
    default:                         return tr("Unknown return code %1").arg(code);
    }
}

// ---------------- 控制包发送 ----------------

void MqttClient::sendConnect()
{
    // 可变头：协议名 "MQTT"(4) + 级别 0x04 + 连接标志 + 保活时间
    QByteArray variableHeader;
    variableHeader += encodeString(QStringLiteral("MQTT"));
    variableHeader.append(static_cast<char>(0x04));   // 协议级别 4 = MQTT 3.1.1

    char connectFlags = 0x02;                          // clean session
    if (!m_username.isEmpty())
        connectFlags |= static_cast<char>(0x80);       // username
    if (!m_password.isEmpty())
        connectFlags |= static_cast<char>(0x40);       // password
    variableHeader.append(connectFlags);
    variableHeader.append(static_cast<char>((m_keepAliveSec >> 8) & 0xFF));
    variableHeader.append(static_cast<char>(m_keepAliveSec & 0xFF));

    // 载荷：clientId (+ username + password)
    QByteArray payload;
    payload += encodeString(m_clientId.isEmpty() ? QStringLiteral("fieldlink") : m_clientId);
    if (!m_username.isEmpty())
        payload += encodeString(m_username);
    if (!m_password.isEmpty())
        payload += encodeString(m_password);

    QByteArray packet;
    packet.append(static_cast<char>(0x10));            // CONNECT
    packet += encodeRemainingLength(variableHeader.size() + payload.size());
    packet += variableHeader;
    packet += payload;
    m_socket->write(packet);
}

void MqttClient::sendPingreq()
{
    m_socket->write(QByteArray(static_cast<const char *>("\xC0\x00"), 2));
}

void MqttClient::sendDisconnect()
{
    m_socket->write(QByteArray(static_cast<const char *>("\xE0\x00"), 2));
}

// ---------------- 发布 ----------------

bool MqttClient::publish(const QString &topic, const QByteArray &payload, bool retain)
{
    if (!m_brokerConnected) {
        // M1：断连期间静默丢弃，只在每个断连周期发一次告警，避免轮询场景日志刷屏
        ++m_droppedCount;
        if (!m_dropWarningEmitted) {
            m_dropWarningEmitted = true;
            emit errorOccurred(tr("MQTT not connected: dropping published messages (%1 dropped, resumes when connected)")
                                   .arg(m_droppedCount));
        }
        return false;
    }

    // QoS1：固定头 0x32|retain（重发时另置 DUP）+ 报文标识符，等待 PUBACK 确认
    if (m_publishQos >= 1) {
        const quint16 packetId = m_nextPacketId;
        m_nextPacketId = static_cast<quint16>((m_nextPacketId % 65535) + 1);   // 跳过保留值 0

        const QByteArray topicBytes = encodeString(topic);
        const int remaining = topicBytes.size() + 2 + payload.size();

        QByteArray packet;
        packet.append(static_cast<char>(0x32 | (retain ? 0x01 : 0x00)));
        packet += encodeRemainingLength(remaining);
        packet += topicBytes;
        packet.append(static_cast<char>((packetId >> 8) & 0xFF));
        packet.append(static_cast<char>(packetId & 0xFF));
        packet += payload;
        m_socket->write(packet);

        PendingPublish pending;
        pending.packet = packet;
        pending.topic = topic;
        pending.payloadSize = payload.size();
        m_pending.insert(packetId, pending);
        if (!m_retransmitTimer.isActive())
            m_retransmitTimer.start();
        return true;   // published 信号延后到收到 PUBACK 时发出
    }

    // PUBLISH QoS0：固定头 0x30|retain + 剩余长度 + 主题(变长字符串) + 载荷
    const QByteArray topicBytes = encodeString(topic);
    const int remaining = topicBytes.size() + payload.size();

    QByteArray packet;
    packet.append(static_cast<char>(0x30 | (retain ? 0x01 : 0x00)));
    packet += encodeRemainingLength(remaining);
    packet += topicBytes;
    packet += payload;
    m_socket->write(packet);

    emit published(topic, payload.size());
    return true;
}

bool MqttClient::publishJson(const QString &topic, const QJsonObject &object, bool retain)
{
    const QJsonDocument doc(object);
    return publish(topic, doc.toJson(QJsonDocument::Compact), retain);
}

// ---------------- 接收解析 ----------------

void MqttClient::onSocketReadyRead()
{
    m_buffer.append(m_socket->readAll());
    processBuffer();
}

void MqttClient::processBuffer()
{
    // 循环解析：一次 read 可能包含多个完整包，也可能只有半包
    while (true) {
        if (m_buffer.size() < 2)
            return;

        // 解析剩余长度 varint（最多 4 字节）
        int remaining = 0;
        int multiplier = 1;
        int headerSize = 1;
        bool complete = false;
        for (int i = 1; i < m_buffer.size() && i <= 4; ++i) {
            const unsigned char byte = static_cast<unsigned char>(m_buffer.at(i));
            remaining += (byte & 0x7F) * multiplier;
            multiplier *= 128;
            headerSize = i + 1;
            if ((byte & 0x80) == 0) {
                complete = true;
                break;
            }
        }
        if (!complete)
            return;                                     // varint 未读完，等待更多数据
        if (m_buffer.size() < headerSize + remaining)
            return;                                     // 包体未收齐

        const unsigned char typeFlags = static_cast<unsigned char>(m_buffer.at(0));
        const int type = typeFlags >> 4;
        const QByteArray body = m_buffer.mid(headerSize, remaining);
        m_buffer.remove(0, headerSize + remaining);

        switch (type) {
        case PKT_CONNACK:
            handleConnack(body);
            break;
        case PKT_PUBLISH:
            // 本客户端不订阅，正常不会收到；忽略
            break;
        case PKT_PUBACK:
            handlePuback(body);
            break;
        case PKT_PINGRESP:
            break;
        case PKT_DISCONNECT:
            // broker 主动断开
            m_socket->disconnectFromHost();
            break;
        default:
            break;
        }
        if (!m_brokerConnected && !m_userRequestedDisconnect
            && m_socket->state() != QAbstractSocket::ConnectedState)
            return;                                     // 连接已失效，停止解析
    }
}

void MqttClient::handleConnack(const QByteArray &body)
{
    if (body.size() < 2) {
        emit errorOccurred(QStringLiteral("MQTT CONNACK 格式非法"));
        m_socket->disconnectFromHost();
        return;
    }
    const int code = static_cast<unsigned char>(body.at(1));
    if (code != CONNACK_ACCEPTED) {
        // 认证/协议问题重试无意义：停止重连
        m_userRequestedDisconnect = true;
        m_reconnectTimer.stop();
        emit errorOccurred(tr("MQTT broker refused connection: %1").arg(connackCodeText(code)));
        m_socket->disconnectFromHost();
        return;
    }

    m_brokerConnected = true;
    m_pingTimer.start();
    emit connected();
}

void MqttClient::handlePuback(const QByteArray &body)
{
    if (body.size() < 2)
        return;
    const quint16 packetId = static_cast<quint16>(
        (static_cast<unsigned char>(body.at(0)) << 8) | static_cast<unsigned char>(body.at(1)));
    const auto it = m_pending.constFind(packetId);
    if (it == m_pending.constEnd())
        return;   // 未知/重复 PUBACK，忽略
    emit published(it->topic, it->payloadSize);
    m_pending.erase(it);
    if (m_pending.isEmpty())
        m_retransmitTimer.stop();
}

void MqttClient::sendPendingRetransmits()
{
    if (!m_brokerConnected || m_pending.isEmpty())
        return;

    QVector<quint16> dropped;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (++it->retries > 5) {   // 重发上限：放弃并告警
            dropped.append(it.key());
            continue;
        }
        QByteArray dup = it->packet;
        dup[0] = static_cast<char>(static_cast<unsigned char>(dup.at(0)) | 0x08);   // DUP=1
        m_socket->write(dup);
    }

    for (const quint16 id : dropped) {
        const auto it = m_pending.constFind(id);
        if (it != m_pending.constEnd()) {
            emit errorOccurred(tr("MQTT QoS1 message dropped after %1 retries (packet id %2)")
                                   .arg(5).arg(id));
            m_pending.erase(it);
        }
    }
    if (m_pending.isEmpty())
        m_retransmitTimer.stop();
}
