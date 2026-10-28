// mqttclient.cpp
// 极简 MQTT 3.1.1 发布端客户端实现。
// 协议参考：MQTT Version 3.1.1 (OASIS Standard)。
// 所有控制包 = 固定头(1 字节类型/标志 + 1~4 字节剩余长度 varint) + 变长头 + 载荷。

#include "mqttclient.h"
#include "mqttoutbox.h"

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
// 每次补发最多发送多少条，避免重连瞬间卡住 UI / 淹没 broker
constexpr int kOutboxFlushBatch = 100;
// 补发节拍：PUBACK 释放窗口后继续补发
constexpr int kOutboxFlushIntervalMs = 300;

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

    m_outboxFlushTimer.setSingleShot(true);
    m_outboxFlushTimer.setInterval(kOutboxFlushIntervalMs);
    connect(&m_outboxFlushTimer, &QTimer::timeout, this, &MqttClient::flushOutbox);
}

MqttClient::~MqttClient()
{
    m_userRequestedDisconnect = true;
    // 进程退出时把未确认的 QoS1 消息留在离线队列，下次启动继续补发。
    requeuePendingToOutbox();
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

bool MqttClient::enableOutbox(const QString &dbPath)
{
    if (m_outbox && m_outbox->isOpen()
        && m_outbox->databasePath() == dbPath) {
        m_outbox->setLimits(m_outboxMaxMessages, m_outboxMaxBytes);
        return true;
    }
    m_outbox.reset(new MqttOutbox());
    m_outbox->setLimits(m_outboxMaxMessages, m_outboxMaxBytes);
    if (!m_outbox->open(dbPath)) {
        const QString error = m_outbox->lastError();
        m_outbox.reset();
        emit errorOccurred(tr("MQTT offline queue open failed: %1").arg(error));
        return false;
    }
    emitQueueSize();
    return true;
}

void MqttClient::disableOutbox()
{
    m_outboxFlushTimer.stop();
    m_outboxInFlight.clear();
    m_outbox.reset();
    emitQueueSize();
}

bool MqttClient::isOutboxEnabled() const
{
    return m_outbox && m_outbox->isOpen();
}

MqttOutbox *MqttClient::outbox() const
{
    return m_outbox.data();
}

void MqttClient::setOutboxLimits(int maxMessages, qint64 maxBytes)
{
    m_outboxMaxMessages = qMax(1, maxMessages);
    m_outboxMaxBytes = qMax<qint64>(1024, maxBytes);
    if (m_outbox)
        m_outbox->setLimits(m_outboxMaxMessages, m_outboxMaxBytes);
    emitQueueSize();
}

int MqttClient::outboxCount() const
{
    return m_outbox ? m_outbox->count() : 0;
}

qint64 MqttClient::outboxBytes() const
{
    return m_outbox ? m_outbox->bytes() : 0;
}

int MqttClient::outboxDroppedCount() const
{
    return m_outbox ? m_outbox->droppedCount() : 0;
}

bool MqttClient::clearOutbox()
{
    if (!m_outbox)
        return false;
    const bool ok = m_outbox->clear();
    m_outboxInFlight.clear();
    // 已经发出、等待 PUBACK 的队列消息也一并放弃重发：
    // 用户点「清空队列」后不应再收到这些历史消息的补发。
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->outboxId > 0)
            it = m_pending.erase(it);
        else
            ++it;
    }
    if (m_pending.isEmpty())
        m_retransmitTimer.stop();
    emitQueueSize();
    return ok;
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
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        requeuePendingToOutbox();
        m_socket->abort();
    }

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
    if (m_brokerConnected) {
        // 主动断开（例如用户关闭 MQTT）也把未确认消息留在离线队列，
        // 下次启用后仍能补发，不因一次手动断开丢数据。
        requeuePendingToOutbox();
        sendDisconnect();
    }
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
    m_outboxFlushTimer.stop();
    m_buffer.clear();

    // QoS1 待确认消息在会话失效后不能静默丢弃：
    //   - 已来自离线队列（outboxId > 0）的消息，数据库行仍在，重连后自然补发；
    //   - 直接发送的消息，先落盘再清空。
    if (!m_pending.isEmpty()) {
        const int count = m_pending.size();
        if (m_outbox && m_outbox->isOpen()) {
            requeuePendingToOutbox();
            emit errorOccurred(tr("MQTT connection lost: %1 unacknowledged message(s) kept in offline queue")
                                   .arg(count));
        } else {
            emit errorOccurred(tr("MQTT connection lost: %1 unacknowledged QoS1 message(s) dropped")
                                   .arg(count));
            m_pending.clear();
        }
    }
    m_retransmitTimer.stop();
    m_dropWarningEmitted = false;   // 新的断连周期允许再次告警
    m_lastPingRespMs = 0;
    m_pingOutstanding = false;
    emitQueueSize();
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

bool MqttClient::publish(const QString &topic, const QByteArray &payload, bool retain, int priority)
{
    // MQTT 字符串长度字段为 2 字节（最大 65535），剩余长度 varint 最大 268435455。
    // 超限直接拒绝，避免产出协议非法的包。
    if (topic.toUtf8().size() > 65535 || payload.size() > kMaxRemainingLength - 65540) {
        emit errorOccurred(tr("MQTT message too large (topic %1 bytes, payload %2 bytes), rejected")
                               .arg(topic.toUtf8().size()).arg(payload.size()));
        return false;
    }

    if (!m_brokerConnected)
        return enqueueToOutbox(topic, payload, retain, m_publishQos, priority);

    if (m_publishQos >= 1) {
        // 在途消息上限：broker 永不回 PUBACK 时 m_pending 会无界增长。
        // 有离线队列时转入队列等待窗口，而不是直接丢弃。
        if (m_pending.size() >= kMaxPendingPublish) {
            if (m_outbox && m_outbox->isOpen())
                return enqueueToOutbox(topic, payload, retain, 1, priority);
            emit errorOccurred(tr("MQTT too many unacknowledged QoS1 messages (%1), message dropped")
                                   .arg(m_pending.size()));
            return false;
        }
        return sendQos1(topic, payload, retain, priority, 0);
    }

    return sendQos0(topic, payload, retain);
}

bool MqttClient::publishJson(const QString &topic, const QJsonObject &object, bool retain, int priority)
{
    const QJsonDocument doc(object);
    return publish(topic, doc.toJson(QJsonDocument::Compact), retain, priority);
}

bool MqttClient::enqueueToOutbox(const QString &topic, const QByteArray &payload,
                                 bool retain, int qos, int priority)
{
    if (!m_outbox || !m_outbox->isOpen()) {
        // 未启用离线队列：保持原有行为，断连期间只在每个周期告警一次。
        ++m_droppedCount;
        if (!m_dropWarningEmitted) {
            m_dropWarningEmitted = true;
            emit errorOccurred(tr("MQTT not connected: dropping published messages (%1 dropped, resumes when connected)")
                                   .arg(m_droppedCount));
        }
        return false;
    }

    qint64 id = 0;
    if (!m_outbox->enqueue(topic, payload, retain, qos, priority, &id)) {
        emit errorOccurred(tr("MQTT offline queue rejected message (queue %1, %2 bytes): %3")
                               .arg(m_outbox->count())
                               .arg(m_outbox->bytes())
                               .arg(m_outbox->lastError()));
        return false;
    }

    emit messageQueued(topic, payload.size(), m_outbox->count());
    emitQueueSize();
    // 已连接但正好处于 QoS1 在途上限时，等窗口释放后由定时器继续补发。
    if (m_brokerConnected)
        m_outboxFlushTimer.start();
    return true;
}

bool MqttClient::sendQos0(const QString &topic, const QByteArray &payload, bool retain)
{
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

bool MqttClient::sendQos1(const QString &topic, const QByteArray &payload, bool retain,
                          int priority, qint64 outboxId)
{
    if (m_pending.size() >= kMaxPendingPublish)
        return false;

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
    pending.payload = payload;
    pending.payloadSize = payload.size();
    pending.qos = 1;
    pending.retain = retain;
    pending.priority = priority;
    pending.outboxId = outboxId;
    pending.firstSentMs = QDateTime::currentMSecsSinceEpoch();
    pending.retries = 0;
    m_pending.insert(packetId, pending);
    if (outboxId > 0)
        m_outboxInFlight.insert(outboxId);
    if (!m_retransmitTimer.isActive())
        m_retransmitTimer.start();
    return true;   // published 信号延后到收到 PUBACK 时发出
}

void MqttClient::flushOutbox()
{
    if (!m_brokerConnected || !m_outbox || !m_outbox->isOpen())
        return;

    const int before = m_outbox->count();
    const QVector<MqttOutboxMessage> batch = m_outbox->peek(kOutboxFlushBatch * 2);
    int sent = 0;
    for (const MqttOutboxMessage &message : batch) {
        if (m_pending.size() >= kMaxPendingPublish)
            break;
        if (m_outboxInFlight.contains(message.id))
            continue;
        if (message.qos >= 1) {
            if (!sendQos1(message.topic, message.payload, message.retain,
                          message.priority, message.id)) {
                break;
            }
        } else {
            if (!sendQos0(message.topic, message.payload, message.retain))
                break;
            if (!m_outbox->remove(message.id))
                break;
        }
        if (++sent >= kOutboxFlushBatch)
            break;
    }

    emitQueueSize();
    const int remaining = m_outbox->count();
    // 仅剩在途消息时不再空转定时器；PUBACK/重发失败会重新触发补发。
    if (remaining > m_outboxInFlight.size() && m_brokerConnected)
        m_outboxFlushTimer.start();
    // QoS0 消息在本次循环里直接删除，只有真正发过消息才宣告补发完成，
    // 避免连接建立时对空队列发一次误报。
    if (remaining == 0 && before > 0)
        emit outboxDrained();
}

void MqttClient::requeuePendingToOutbox()
{
    if (m_pending.isEmpty())
        return;
    if (!m_outbox || !m_outbox->isOpen()) {
        m_pending.clear();
        return;
    }

    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        // 来自离线队列的消息，数据库行还在（PUBACK 后才删除），无需重复入队。
        if (it->outboxId > 0)
            continue;
        m_outbox->enqueue(it->topic, it->payload, it->retain, it->qos, it->priority);
    }
    m_pending.clear();
    m_outboxInFlight.clear();
    m_retransmitTimer.stop();
    emitQueueSize();
}

void MqttClient::emitQueueSize()
{
    emit queueSizeChanged(m_outbox ? m_outbox->count() : 0);
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
        emit errorOccurred(tr("MQTT CONNACK frame is invalid"));
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
    // 先补发离线队列中的历史消息（报警优先），再通知上层"已连接"。
    flushOutbox();
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
    if (pending.outboxId > 0) {
        if (m_outbox) {
            m_outbox->remove(pending.outboxId);
            emitQueueSize();
        }
        m_outboxInFlight.remove(pending.outboxId);
        // QoS1 的 outbox 行在 PUBACK 时才删除，补发完成的判定必须放在这里。
        if (m_outbox && m_outbox->count() == 0 && m_outboxInFlight.isEmpty())
            emit outboxDrained();
    }
    if (m_pending.isEmpty())
        m_retransmitTimer.stop();
    // 释放了一个在途窗口，继续补发离线队列。
    if (m_outbox && m_outbox->count() > 0 && m_brokerConnected)
        m_outboxFlushTimer.start();
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
        const PendingPublish pending = m_pending.value(id);
        if (pending.outboxId > 0) {
            if (m_outbox)
                m_outbox->remove(pending.outboxId);
            m_outboxInFlight.remove(pending.outboxId);
        }
        m_pending.remove(id);
    }
    if (!dropped.isEmpty())
        emitQueueSize();
    if (m_pending.isEmpty())
        m_retransmitTimer.stop();
    if (m_outbox && m_outbox->count() > 0 && m_brokerConnected)
        m_outboxFlushTimer.start();
}
