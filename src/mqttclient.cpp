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
constexpr int kConnectTimeoutMs = 10 * 1000;   // 连接超时：10s 内未收到 CONNACK 即断开重试

// QoS1 重发间隔（MQTT 3.1.1 §4.3.3 建议重发间隔）
constexpr int kRetransmitIntervalMs = 2 * 1000;
// QoS1 重发次数上限，超过则放弃并告警
constexpr int kMaxRetransmits = 5;
// 单个控制包的最大剩余长度（MQTT 3.1.1 允许到 256MB，但现场 broker 无需这么大）。
// 设置硬上限可防止畸形/恶意长度字段导致客户端缓冲数百 MB（内存耗尽）。
constexpr int kMaxRemainingLength = 16 * 1024 * 1024;   // 16 MB
// 接收缓冲区上限：超过即判定为协议异常并断开
constexpr int kMaxBufferSize = 32 * 1024 * 1024;        // 32 MB
// QoS1 在途消息上限：防止 broker 永不回 PUBACK 时 m_pending 无界增长
constexpr int kMaxPendingPublish = 1000;

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

    m_connectTimeoutTimer.setSingleShot(true);
    m_connectTimeoutTimer.setInterval(kConnectTimeoutMs);
    connect(&m_connectTimeoutTimer, &QTimer::timeout, this, &MqttClient::onConnectTimeout);
}

MqttClient::~MqttClient()
{
    m_userRequestedDisconnect = true;
    if (m_brokerConnected) {
        sendDisconnect();
        // socket 随后作为子对象被销毁，未 flush 的数据不会发出，
        // broker 侧看到的是 TCP 异常关闭而非 DISCONNECT。这里补一次 flush。
        m_socket->flush();
        m_socket->waitForBytesWritten(200);
    }
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
    // CONNECT 报文的 Keep Alive 字段为 2 字节（最大 65535），超出会被静默截断
    m_keepAliveSec = qBound(1, seconds, 65535);
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
    // 任何非未连接状态（连接中/已连接/半开连接）一律断开重来，
    // 避免"卡在正在连接"或"半开连接上静默返回"的死状态
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();

    m_userRequestedDisconnect = false;
    m_brokerConnected = false;
    m_buffer.clear();
    emit errorOccurred(tr("MQTT connecting to %1 ...").arg(brokerInfo()));
    startConnectAttempt();
}

void MqttClient::disconnectFromBroker()
{
    m_userRequestedDisconnect = true;
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    m_connectTimeoutTimer.stop();
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
    // 必须与 connectToBroker() 走同一条建连路径：否则重连后没有连接超时保护，
    // broker 收了 TCP 却不回 CONNACK 时客户端会永久卡在"连接中"且不再重试。
    startConnectAttempt();
}

// 建连 + 等待 CONNACK 的统一入口。
// 连接超时定时器在这里启动 —— 手动连接与自动重连共用，保证两条路径都有超时保护。
void MqttClient::startConnectAttempt()
{
    m_connectTimeoutTimer.start();
    m_socket->connectToHost(m_host, m_port);
}

void MqttClient::onPingTimer()
{
    if (!m_brokerConnected)
        return;

    // PINGRESP 看门狗（MQTT 3.1.1 §3.1.2.10）：
    // 客户端发出 PINGREQ 后，若在 1.5 × keepAlive 内未收到 PINGRESP，
    // 必须关闭连接。否则 broker 进程假死（TCP 未断）时客户端会一直认为"已连接"，
    // PUBLISH 不断写入黑洞、写缓冲与在途队列持续增长。
    if (m_pingOutstanding && m_lastPingRespMs > 0) {
        const qint64 deadline = m_lastPingRespMs + static_cast<qint64>(m_keepAliveSec) * 1500;
        if (QDateTime::currentMSecsSinceEpoch() > deadline) {
            emit errorOccurred(tr("MQTT no PINGRESP within %1 s, reconnecting")
                                   .arg(static_cast<double>(m_keepAliveSec) * 1.5));
            m_socket->abort();
            return;
        }
    }
    sendPingreq();
}

// 收到 PINGRESP：记录时间戳并清除"等待应答"标志
void MqttClient::onPingResp()
{
    m_lastPingRespMs = QDateTime::currentMSecsSinceEpoch();
    m_pingOutstanding = false;
}

void MqttClient::resetSessionState()
{
    m_brokerConnected = false;
    m_pingTimer.stop();
    m_connectTimeoutTimer.stop();
    m_buffer.clear();
    // 断线即清空 QoS1 待确认队列（会话已失效）。但必须告警：
    // 否则调用方只知道 publish() 返回了 true，实际消息已永久丢失（静默丢数据）。
    if (!m_pending.isEmpty()) {
        emit errorOccurred(tr("MQTT connection lost: %1 unacknowledged QoS1 message(s) dropped")
                               .arg(m_pending.size()));
        m_pending.clear();
    }
    m_retransmitTimer.stop();
    m_dropWarningEmitted = false;   // 新的断连周期允许再次告警
    m_lastPingRespMs = 0;
    m_pingOutstanding = false;
}

// 连接超时：TCP 已连上但 CONNACK 迟迟不来（broker 假死/被静默过滤等），
// 或 TCP 握手本身挂起。断开并按自动重连策略重试，状态不再永久卡死。
void MqttClient::onConnectTimeout()
{
    if (m_brokerConnected || m_userRequestedDisconnect)
        return;
    emit errorOccurred(tr("MQTT connect timeout: no CONNACK within %1 s, retrying...")
                           .arg(kConnectTimeoutMs / 1000));
    m_socket->abort();
    scheduleReconnect();
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
    // MQTT 字符串长度字段为 2 字节，最大 65535。超长必须由调用方拒绝，
    // 否则长度字段被截断会产出非法包（这里截断到上限并由调用方校验）。
    const int len = qMin(utf8.size(), 65535);
    out.append(static_cast<char>((len >> 8) & 0xFF));
    out.append(static_cast<char>(len & 0xFF));
    out.append(utf8.left(len));
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
    m_lastPingRespMs = QDateTime::currentMSecsSinceEpoch();
    m_pingOutstanding = true;
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

    // MQTT 字符串长度字段为 2 字节（最大 65535），剩余长度 varint 最大 268435455。
    // 超限直接拒绝，避免产出协议非法的包。
    if (topic.toUtf8().size() > 65535 || payload.size() > kMaxRemainingLength - 65540) {
        emit errorOccurred(tr("MQTT message too large (topic %1 bytes, payload %2 bytes), rejected")
                               .arg(topic.toUtf8().size()).arg(payload.size()));
        return false;
    }

    // QoS1：固定头 0x32|retain（重发时另置 DUP）+ 报文标识符，等待 PUBACK 确认
    if (m_publishQos >= 1) {
        // 在途消息上限：broker 永不回 PUBACK 时 m_pending 会无界增长
        if (m_pending.size() >= kMaxPendingPublish) {
            emit errorOccurred(tr("MQTT too many unacknowledged QoS1 messages (%1), message dropped")
                                   .arg(m_pending.size()));
            return false;
        }

        // 报文标识符分配：必须跳过仍在等待 PUBACK 的 ID。
        // MQTT 3.1.1 §3.3.1.4 规定不得对未确认报文复用 Packet Identifier；
        // 直接复用会覆盖 m_pending 中的旧消息，导致它既不会被重发、
        // 也永远不会触发 published 信号 —— 数据静默丢失。
        quint16 packetId = m_nextPacketId;
        for (int guard = 0; guard < 65535; ++guard) {
            if (!m_pending.contains(packetId))
                break;
            packetId = static_cast<quint16>((packetId % 65535) + 1);   // 跳过保留值 0
        }
        if (m_pending.contains(packetId)) {
            // 65535 条全部在途：无法再分配新 ID
            emit errorOccurred(tr("MQTT no free packet identifier available, message dropped"));
            return false;
        }
        m_nextPacketId = static_cast<quint16>((packetId % 65535) + 1);   // 跳过保留值 0

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
        pending.firstSentMs = QDateTime::currentMSecsSinceEpoch();
        pending.retries = 0;
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
        // 缓冲区上限保护：超过即视为协议异常（畸形长度字段或恶意 broker），
        // 直接断开重连，避免内存被耗尽。
        if (m_buffer.size() > kMaxBufferSize) {
            emit errorOccurred(tr("MQTT receive buffer overflow (%1 bytes), disconnecting")
                                   .arg(m_buffer.size()));
            m_socket->abort();
            return;
        }

        if (m_buffer.size() < 2)
            return;

        // 解析剩余长度 varint（最多 4 字节）
        int remaining = 0;
        int multiplier = 1;
        int headerSize = 1;
        bool complete = false;
        bool malformed = false;
        for (int i = 1; i < m_buffer.size() && i <= 4; ++i) {
            const unsigned char byte = static_cast<unsigned char>(m_buffer.at(i));
            remaining += (byte & 0x7F) * multiplier;
            multiplier *= 128;
            headerSize = i + 1;
            if ((byte & 0x80) == 0) {
                complete = true;
                break;
            }
            // 4 个字节后仍有延续位 → 违反 MQTT 3.1.1 §2.2.3（varint 最多 4 字节）。
            // 此时 complete 永远为 false，若继续等待更多数据，这几个字节永远不会被消费，
            // 缓冲区会单调增长且解析器彻底僵死（后续合法包也不再被处理）。
            if (i == 4) {
                malformed = true;
                break;
            }
        }
        if (malformed) {
            emit errorOccurred(tr("MQTT malformed remaining-length field, disconnecting"));
            m_buffer.clear();
            m_socket->abort();
            return;
        }
        if (!complete)
            return;                                     // varint 未读完，等待更多数据

        // 单包长度上限：防止声明 256MB 长度导致缓冲数百 MB
        if (remaining > kMaxRemainingLength) {
            emit errorOccurred(tr("MQTT packet too large (%1 bytes, limit %2), disconnecting")
                                   .arg(remaining).arg(kMaxRemainingLength));
            m_buffer.clear();
            m_socket->abort();
            return;
        }

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
            onPingResp();
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
    m_connectTimeoutTimer.stop();   // 收到 CONNACK：连接阶段结束（无论接受与否）
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
    if (!m_pending.contains(packetId))
        return;   // 未知/重复 PUBACK，忽略
    const PendingPublish pending = m_pending.value(packetId);
    emit published(pending.topic, pending.payloadSize);
    // 注意：QMap::erase 只接受 iterator（const_iterator 不可用），直接按 key 移除
    m_pending.remove(packetId);
    if (m_pending.isEmpty())
        m_retransmitTimer.stop();
}

void MqttClient::sendPendingRetransmits()
{
    if (!m_brokerConnected || m_pending.isEmpty())
        return;

    // 只有真正到达重发间隔的消息才重发。
    // 之前所有在途消息共用一个全局定时器节拍，导致刚发布几毫秒的新消息
    // 立刻被重发并打上 DUP=1 —— 违反 MQTT 3.1.1 §3.3.1.1（首次投递必须 DUP=0），
    // broker 会把它当新消息再投递给订阅者，造成重复数据。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QVector<quint16> dropped;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (now - it->firstSentMs < kRetransmitIntervalMs)
            continue;                       // 尚未到重发时间，保持 DUP=0
        if (++it->retries > kMaxRetransmits) {   // 重发上限：放弃并告警
            dropped.append(it.key());
            continue;
        }
        QByteArray dup = it->packet;
        dup[0] = static_cast<char>(static_cast<unsigned char>(dup.at(0)) | 0x08);   // DUP=1
        m_socket->write(dup);
    }

    for (const quint16 id : dropped) {
        emit errorOccurred(tr("MQTT QoS1 message dropped after %1 retries (packet id %2)")
                               .arg(kMaxRetransmits).arg(id));
        m_pending.remove(id);
    }
    if (m_pending.isEmpty())
        m_retransmitTimer.stop();
}
