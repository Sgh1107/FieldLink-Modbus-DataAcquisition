// mqttclient.cpp
// 极简 MQTT 3.1.1 发布端客户端实现。
// 协议参考：MQTT Version 3.1.1 (OASIS Standard)。
// 所有控制包 = 固定头(1 字节类型/标志 + 1~4 字节剩余长度 varint) + 变长头 + 载荷。

#include "mqttclient.h"

#include <QDateTime>
#include <QJsonDocument>

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
        emit errorOccurred(QStringLiteral("MQTT broker 地址未配置"));
        return;
    }
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        return;                                     // 连接中/已连接：忽略重复请求

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
        emit errorOccurred(QStringLiteral("MQTT 连接错误: %1").arg(m_socket->errorString()));
}

void MqttClient::onReconnectTimer()
{
    if (m_userRequestedDisconnect || m_socket->state() != QAbstractSocket::UnconnectedState)
        return;
    emit errorOccurred(QStringLiteral("MQTT 正在重连 %1 ...").arg(brokerInfo()));
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
    case CONNACK_ACCEPTED:           return QStringLiteral("连接被接受");
    case CONNACK_BAD_PROTOCOL:       return QStringLiteral("不支持的协议版本");
    case CONNACK_CLIENT_REJECTED:    return QStringLiteral("客户端标识符被拒绝");
    case CONNACK_SERVER_UNAVAILABLE: return QStringLiteral("服务端不可用");
    case CONNACK_BAD_CREDENTIALS:    return QStringLiteral("用户名或密码错误");
    case CONNACK_NOT_AUTHORIZED:     return QStringLiteral("未授权");
    default:                         return QStringLiteral("未知返回码 %1").arg(code);
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
        emit errorOccurred(QStringLiteral("MQTT 未连接，放弃发布: %1").arg(topic));
        return false;
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
        emit errorOccurred(QStringLiteral("MQTT broker 拒绝连接: %1").arg(connackCodeText(code)));
        m_socket->disconnectFromHost();
        return;
    }

    m_brokerConnected = true;
    m_pingTimer.start();
    emit connected();
}
