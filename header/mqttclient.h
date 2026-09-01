#ifndef MQTTCLIENT_H
#define MQTTCLIENT_H

// mqttclient.h
// 极简 MQTT 3.1.1 发布端客户端（零外部依赖，基于 QTcpSocket）。
//
// 能力边界（刻意保持精简，降低出错面）：
//   - 仅实现发布端所需的协议子集：CONNECT / CONNACK / PUBLISH(QoS0) /
//     PINGREQ / PINGRESP / DISCONNECT
//   - QoS 0（遥测高频数据常用级别，不做 PUBACK/重发队列）
//   - 自动重连（broker 断开后每 5 秒重试；认证被拒时不重试）
//   - MQTT 3.1.1（协议级别 4），兼容 mosquitto / EMQX / Mosca 等主流 broker
//
// 测试：deploy/mqtt_test_broker.py 提供标准库实现的迷你 broker，
//       可对 CONNECT/PUBLISH 字节流做离线验证（见 doc/MQTT_GUIDE.md）。

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QByteArray>
#include <QString>

class MqttClient : public QObject
{
    Q_OBJECT

public:
    explicit MqttClient(QObject *parent = nullptr);
    ~MqttClient();

    void setBroker(const QString &host, quint16 port);
    void setCredentials(const QString &clientId,
                        const QString &username = QString(),
                        const QString &password = QString());
    void setKeepAlive(int seconds);            // 默认 60
    void setAutoReconnect(bool enabled);       // 默认 true

    bool isConnectedToBroker() const;          // 已完成 CONNACK 握手
    QString brokerInfo() const;                // "host:port"

public slots:
    void connectToBroker();
    void disconnectFromBroker();
    // QoS0 发布；未连接时返回 false 并发 errorOccurred
    bool publish(const QString &topic, const QByteArray &payload, bool retain = false);
    bool publishJson(const QString &topic, const QJsonObject &object, bool retain = false);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void published(const QString &topic, int payloadBytes);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onPingTimer();
    void onReconnectTimer();

private:
    // MQTT 3.1.1 控制包类型（本客户端关注的子集）
    enum PacketType {
        PKT_CONNACK    = 2,
        PKT_PUBLISH    = 3,
        PKT_PUBACK     = 4,
        PKT_PINGRESP   = 13,
        PKT_DISCONNECT = 14
    };

    void scheduleReconnect();
    void sendConnect();
    void sendPingreq();
    void sendDisconnect();
    void processBuffer();
    void handleConnack(const QByteArray &body);
    void resetSessionState();

    static QByteArray encodeRemainingLength(int length);
    static QByteArray encodeString(const QString &text);
    static QString connackCodeText(int code);

    QTcpSocket *m_socket;
    QByteArray m_buffer;
    QString m_host;
    quint16 m_port;
    QString m_clientId;
    QString m_username;
    QString m_password;
    int m_keepAliveSec;
    bool m_autoReconnect;
    bool m_brokerConnected;       // CONNACK 已确认
    bool m_userRequestedDisconnect;
    QTimer m_pingTimer;           // keepalive/2 发送 PINGREQ
    QTimer m_reconnectTimer;      // 断线后 5 秒重连
};

#endif // MQTTCLIENT_H
