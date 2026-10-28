#ifndef MQTTOUTBOX_H
#define MQTTOUTBOX_H

// mqttoutbox.h
// MQTT 断线离线队列（SQLite 持久化）。
//
// 特性：
//   - 断线/在途消息先落盘，重连后按优先级补发（报警 > 状态 > 遥测）；
//   - 容量上限（条数 + 字节数），超限时优先淘汰低优先级/最旧消息；
//   - 独立数据库文件，进程重启后仍可补发；
//   - 只做存取，不依赖 MqttClient / GUI，便于单元测试。

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

struct MqttOutboxMessage {
    qint64 id = 0;
    QString topic;
    QByteArray payload;
    bool retain = false;
    int qos = 0;
    int priority = 0;          // 0=遥测, 1=状态, 2=报警
    qint64 createdAtMs = 0;
};

class MqttOutbox
{
public:
    enum Priority {
        Telemetry = 0,
        Status = 1,
        Alarm = 2
    };

    explicit MqttOutbox(const QString &dbPath = QString());
    ~MqttOutbox();

    bool open(const QString &dbPath);
    void close();
    bool isOpen() const;
    QString databasePath() const;
    QString lastError() const;

    // maxMessages / maxBytes 至少为 1 / 1KB；超限时按 priority ASC, id ASC 淘汰。
    void setLimits(int maxMessages, qint64 maxBytes);
    int maxMessages() const { return m_maxMessages; }
    qint64 maxBytes() const { return m_maxBytes; }

    // 入队。返回 false 表示数据库不可用，或该消息刚入队就被容量策略淘汰。
    bool enqueue(const QString &topic, const QByteArray &payload, bool retain,
                 int qos, int priority, qint64 *outId = nullptr);

    // 按 priority DESC, id ASC 取前 maxCount 条（报警优先，其次按时间 FIFO）。
    QVector<MqttOutboxMessage> peek(int maxCount) const;
    bool remove(qint64 id);
    bool clear();

    int count() const;
    qint64 bytes() const;
    int droppedCount() const { return m_droppedCount; }

private:
    bool ensureSchema();
    bool trimToLimits();
    void setError(const QString &context, const QString &detail);
    void clearError();

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_dbPath;
    QString m_lastError;
    bool m_schemaReady = false;
    int m_maxMessages = 10000;
    qint64 m_maxBytes = 32LL * 1024 * 1024;
    int m_droppedCount = 0;
};

#endif // MQTTOUTBOX_H
