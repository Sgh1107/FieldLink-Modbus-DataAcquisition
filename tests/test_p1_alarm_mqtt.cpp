// test_p1_alarm_mqtt.cpp
// P1 功能回归：报警持久化 / MQTT 离线队列与补发。
//
// 覆盖：
//   P1A-*  报警规则、事件、确认、活动状态、删除/清空、重启恢复、ID 不复用、数据库故障降级
//   P1M-*  outbox 存取/优先级/容量淘汰/重启持久化；MqttClient 断线入队、
//          重连补发、QoS1 在途消息断线重入队、队列关闭时保持旧行为
//
// 约定：入口 runP1AlarmMqttTests()，由 master_tests.cpp 调用。

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <cstdio>
#include <functional>

#include "alarmmanager.h"
#include "alarmrepository.h"
#include "mqttclient.h"
#include "mqttoutbox.h"

static int g_p1Passed = 0;
static int g_p1Failed = 0;

#define P1_CHECK(name, cond) do { \
    if (cond) { printf("  [PASS] %s\n", name); ++g_p1Passed; } \
    else { printf("  [FAIL] %s (%s:%d)\n", name, __FILE__, __LINE__); ++g_p1Failed; } \
} while (0)

static bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

static void waitMs(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// ---------------- 报警持久化 ----------------

static AlarmRule makeP1Rule(const QString &name = QStringLiteral("高温"))
{
    AlarmRule rule;
    rule.name = name;
    rule.enabled = true;
    rule.deviceId = 7;
    rule.pointId = 42;
    rule.serverAddress = 3;
    rule.registerType = 4;
    rule.address = 100;
    rule.condition = AlarmCondition::GreaterThan;
    rule.threshold1 = 80.0;
    rule.threshold2 = 0.0;
    rule.severity = AlarmSeverity::Critical;
    rule.message = QStringLiteral("温度过高");
    rule.debounceMs = 0;
    return rule;
}

static void testAlarmPersistenceBasic()
{
    printf("\n=== P1 报警持久化：规则/事件/确认/重启恢复 ===\n");
    QTemporaryDir dir;
    P1_CHECK("P1A-01: 临时目录可用", dir.isValid());
    const QString path = dir.path() + QStringLiteral("/alarms.db");

    int ruleId = 0;
    qint64 eventId = 0;

    {
        AlarmRepository repo;
        P1_CHECK("P1A-02: 打开报警数据库", repo.open(path));

        AlarmManager manager;
        manager.setRepository(&repo);
        const AlarmRule rule = makeP1Rule();
        ruleId = manager.addRule(rule);
        P1_CHECK("P1A-03: 新增规则返回有效 ID", ruleId > 0);
        P1_CHECK("P1A-04: 规则写入数据库", repo.ruleCount() == 1);

        const QVector<AlarmRule> storedRules = repo.loadRules();
        P1_CHECK("P1A-05: 规则字段往返", storedRules.size() == 1
                 && storedRules.first().name == rule.name
                 && storedRules.first().enabled
                 && storedRules.first().deviceId == 7
                 && storedRules.first().pointId == 42
                 && storedRules.first().serverAddress == 3
                 && storedRules.first().registerType == 4
                 && storedRules.first().address == 100
                 && storedRules.first().condition == AlarmCondition::GreaterThan
                 && qFuzzyCompare(storedRules.first().threshold1, 80.0)
                 && storedRules.first().severity == AlarmSeverity::Critical
                 && storedRules.first().message == rule.message);

        int triggered = 0;
        AlarmEvent lastEvent;
        QObject::connect(&manager, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &event) { ++triggered; lastEvent = event; });
        manager.checkPointValue(7, 42, 85.5, 0);
        P1_CHECK("P1A-06: 报警正常触发", triggered == 1);
        P1_CHECK("P1A-07: 报警事件写入数据库", repo.eventCount() == 1);

        const QVector<AlarmEvent> events = repo.loadEvents(10);
        P1_CHECK("P1A-08: 事件字段与上下文往返", events.size() == 1
                 && events.first().ruleId == ruleId
                 && events.first().ruleName == rule.name
                 && events.first().severity == AlarmSeverity::Critical
                 && qFuzzyCompare(events.first().value, 85.5)
                 && events.first().deviceId == 7
                 && events.first().pointId == 42
                 && events.first().serverAddress == 3
                 && events.first().registerType == 4
                 && events.first().address == 100
                 && events.first().timestamp.isValid());
        if (!events.isEmpty())
            eventId = events.first().id;

        manager.acknowledgeAlarm(eventId);
        const QVector<AlarmEvent> acked = repo.loadEvents(10);
        P1_CHECK("P1A-09: 确认状态与确认时间落库", acked.size() == 1
                 && acked.first().acknowledged
                 && acked.first().acknowledgedTime.isValid());

        AlarmRule updated = storedRules.first();
        updated.name = QStringLiteral("高温-2");
        updated.threshold1 = 90.0;
        updated.severity = AlarmSeverity::Info;
        manager.updateRule(updated);
        const QVector<AlarmRule> updatedRules = repo.loadRules();
        P1_CHECK("P1A-10: 修改规则落库", updatedRules.size() == 1
                 && updatedRules.first().name == QStringLiteral("高温-2")
                 && qFuzzyCompare(updatedRules.first().threshold1, 90.0)
                 && updatedRules.first().severity == AlarmSeverity::Info);

        manager.setRuleEnabled(ruleId, false);
        const QVector<AlarmRule> disabledRules = repo.loadRules();
        P1_CHECK("P1A-11: 启用/禁用状态落库", disabledRules.size() == 1
                 && !disabledRules.first().enabled);

        // 同一数据库上重建 manager，模拟进程重启。
        AlarmManager restarted;
        int restartedTriggered = 0;
        int restartedCleared = 0;
        QObject::connect(&restarted, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++restartedTriggered; });
        QObject::connect(&restarted, &AlarmManager::alarmCleared,
                         [&](int) { ++restartedCleared; });
        restarted.setRepository(&repo);

        P1_CHECK("P1A-12: 重启后规则恢复", restarted.allRules().size() == 1);
        P1_CHECK("P1A-13: 重启后规则 ID 保持不变", restarted.allRules().first().id == ruleId);
        P1_CHECK("P1A-14: 重启后事件恢复", restarted.alarmHistory(10).size() == 1);
        P1_CHECK("P1A-15: 重启后确认状态恢复",
                 restarted.alarmHistory(10).size() == 1
                 && restarted.alarmHistory(10).first().acknowledged);

        restarted.checkPointValue(7, 42, 95.0, 0);
        P1_CHECK("P1A-16: 禁用规则重启后仍不触发", restartedTriggered == 0);

        restarted.setRuleEnabled(ruleId, true);
        restarted.checkPointValue(7, 42, 95.0, 0);
        P1_CHECK("P1A-17: 重新启用后正常触发", restartedTriggered == 1);
        restarted.checkPointValue(7, 42, 10.0, 0);
        P1_CHECK("P1A-18: 恢复后正常清除", restartedCleared == 1);
    }

    AlarmRepository reopened;
    P1_CHECK("P1A-19: 关闭后重新打开数据库", reopened.open(path));
    P1_CHECK("P1A-20: 磁盘规则仍存在", reopened.ruleCount() == 1);
    P1_CHECK("P1A-21: 磁盘事件仍存在", reopened.eventCount() == 2);
}

static void testAlarmStateRestore()
{
    printf("\n=== P1 报警持久化：活动状态恢复（避免重启重复报警） ===\n");
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/alarms.db");

    int ruleId = 0;
    {
        AlarmRepository repo;
        repo.open(path);
        AlarmManager manager;
        manager.setRepository(&repo);
        ruleId = manager.addRule(makeP1Rule(QStringLiteral("压力")));
        manager.checkPointValue(7, 42, 99.0, 0);
        P1_CHECK("P1A-22: 报警处于活动状态", manager.activeAlarmCount() == 1);
        P1_CHECK("P1A-23: 活动状态写入数据库", repo.loadStates().size() == 1
                 && repo.loadStates().first().active);

        AlarmManager restarted;
        int triggered = 0;
        int cleared = 0;
        QObject::connect(&restarted, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++triggered; });
        QObject::connect(&restarted, &AlarmManager::alarmCleared,
                         [&](int) { ++cleared; });
        restarted.setRepository(&repo);
        P1_CHECK("P1A-24: 重启后活动报警计数恢复", restarted.activeAlarmCount() == 1);
        P1_CHECK("P1A-25: 重启后活动报警可查询", restarted.activeAlarms().size() == 1);

        restarted.checkPointValue(7, 42, 99.0, 0);
        P1_CHECK("P1A-26: 同一报警不会重复触发", triggered == 0);
        P1_CHECK("P1A-27: 重启后报警历史仍为 1 条", restarted.alarmHistory(10).size() == 1);

        restarted.checkPointValue(7, 42, 10.0, 0);
        P1_CHECK("P1A-28: 恢复后条件消失正常清除", cleared == 1);
        P1_CHECK("P1A-29: 清除状态落库", repo.loadStates().first().active == false);

        const int newRuleId = restarted.addRule(makeP1Rule(QStringLiteral("新规则")));
        P1_CHECK("P1A-30: 重启后新规则 ID 不复用", newRuleId > ruleId);
        // 两条规则都命中同一设备/点位，因此这次会触发两条新事件。
        restarted.checkPointValue(7, 42, 99.0, 0);
        const QVector<AlarmEvent> events = repo.loadEvents(10);
        bool idsAscending = true;
        for (int i = 1; i < events.size(); ++i) {
            if (events[i].id <= events[i - 1].id)
                idsAscending = false;
        }
        P1_CHECK("P1A-31: 重启后新事件 ID 不复用",
                 events.size() == 3 && idsAscending);
    }
}

static void testAlarmDeleteAndClear()
{
    printf("\n=== P1 报警持久化：删除规则 / 清空历史 ===\n");
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/alarms.db");

    AlarmRepository repo;
    P1_CHECK("P1A-32: 打开数据库", repo.open(path));
    AlarmManager manager;
    manager.setRepository(&repo);

    const int ruleA = manager.addRule(makeP1Rule(QStringLiteral("规则A")));
    const int ruleB = manager.addRule(makeP1Rule(QStringLiteral("规则B")));
    manager.checkPointValue(7, 42, 90.0, 0);
    manager.checkPointValue(7, 42, 91.0, 0);
    P1_CHECK("P1A-33: 两条规则/两条事件已落库",
             repo.ruleCount() == 2 && repo.eventCount() == 2);

    manager.removeRule(ruleA);
    P1_CHECK("P1A-34: 删除规则后规则数减少", repo.ruleCount() == 1);
    P1_CHECK("P1A-35: 删除规则时级联删除其事件", repo.eventCount() == 1);
    P1_CHECK("P1A-36: 删除规则时级联删除其状态",
             repo.loadStates().size() == 1 && repo.loadStates().first().ruleId == ruleB);

    manager.clearHistory();
    P1_CHECK("P1A-37: 清空历史只删事件不删规则",
             repo.eventCount() == 0 && repo.ruleCount() == 1);
    P1_CHECK("P1A-38: 清空后规则仍可触发",
             manager.alarmHistory(10).isEmpty());
}

static void testAlarmRepositoryFailure()
{
    printf("\n=== P1 报警持久化：数据库不可用时的降级 ===\n");
    QTemporaryDir dir;
    QFile blocker(dir.path() + QStringLiteral("/notadir"));
    P1_CHECK("P1A-39: 创建阻塞文件", blocker.open(QIODevice::WriteOnly));
    blocker.close();

    AlarmRepository repo;
    P1_CHECK("P1A-40: 非法路径打开失败", !repo.open(blocker.fileName()
                                                     + QStringLiteral("/alarms.db")));
    P1_CHECK("P1A-41: 失败原因可读", !repo.lastError().isEmpty());

    AlarmManager manager;
    manager.setRepository(&repo);
    const int id = manager.addRule(makeP1Rule());
    P1_CHECK("P1A-42: 数据库不可用时规则仍可内存使用", id > 0
             && manager.allRules().size() == 1);
    manager.checkPointValue(7, 42, 90.0, 0);
    P1_CHECK("P1A-43: 数据库不可用时报警仍可触发",
             manager.alarmHistory(10).size() == 1);
}

// ---------------- MQTT 离线队列 ----------------

static void testMqttOutboxBasics()
{
    printf("\n=== P1 MQTT 离线队列：存取/优先级/容量/持久化 ===\n");
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/outbox.db");

    qint64 telemetryId = 0;
    {
        MqttOutbox outbox(path);
        P1_CHECK("P1M-01: 打开 outbox 数据库", outbox.isOpen());
        outbox.setLimits(10, 1024 * 1024);

        P1_CHECK("P1M-02: 遥测入队", outbox.enqueue(QStringLiteral("t/telemetry"),
                                                    QByteArray("T"), false, 0,
                                                    MqttOutbox::Telemetry, &telemetryId));
        P1_CHECK("P1M-03: 状态入队", outbox.enqueue(QStringLiteral("t/status"),
                                                    QByteArray("S"), true, 1,
                                                    MqttOutbox::Status));
        P1_CHECK("P1M-04: 报警入队", outbox.enqueue(QStringLiteral("t/alarm"),
                                                    QByteArray("A"), false, 1,
                                                    MqttOutbox::Alarm));
        P1_CHECK("P1M-05: 队列计数正确", outbox.count() == 3);
        P1_CHECK("P1M-06: 队列字节数大于 0", outbox.bytes() > 0);

        const QVector<MqttOutboxMessage> ordered = outbox.peek(10);
        P1_CHECK("P1M-07: 补发顺序为报警 > 状态 > 遥测", ordered.size() == 3
                 && ordered[0].priority == MqttOutbox::Alarm
                 && ordered[1].priority == MqttOutbox::Status
                 && ordered[2].priority == MqttOutbox::Telemetry);
        P1_CHECK("P1M-08: retain/qos 元数据保留",
                 ordered[1].retain && ordered[1].qos == 1);

        P1_CHECK("P1M-09: 按 ID 删除", outbox.remove(telemetryId) && outbox.count() == 2);
        P1_CHECK("P1M-10: 清空队列", outbox.clear() && outbox.count() == 0);
    }

    // 重启后仍能读到消息
    {
        MqttOutbox first(path);
        first.open(path);
        first.enqueue(QStringLiteral("t/persist"), QByteArray("P"), false, 1,
                      MqttOutbox::Telemetry);
    }
    {
        MqttOutbox second(path);
        P1_CHECK("P1M-11: 重启后队列内容仍在", second.isOpen() && second.count() == 1);
        const auto messages = second.peek(1);
        P1_CHECK("P1M-12: 重启后消息内容正确", messages.size() == 1
                 && messages.first().topic == QStringLiteral("t/persist")
                 && messages.first().payload == QByteArray("P"));
    }
}

static void testMqttOutboxLimits()
{
    printf("\n=== P1 MQTT 离线队列：容量淘汰策略 ===\n");
    QTemporaryDir dir;

    {
        MqttOutbox outbox(dir.path() + QStringLiteral("/limit_count.db"));
        outbox.setLimits(3, 1024 * 1024);
        for (int i = 0; i < 5; ++i) {
            outbox.enqueue(QStringLiteral("t/%1").arg(i), QByteArray("x"), false, 0,
                           MqttOutbox::Telemetry);
        }
        P1_CHECK("P1M-13: 超过条数上限后只保留上限条数", outbox.count() == 3);
        const auto messages = outbox.peek(10);
        P1_CHECK("P1M-14: 淘汰最旧的遥测", messages.size() == 3
                 && messages.first().topic == QStringLiteral("t/2")
                 && messages.last().topic == QStringLiteral("t/4"));

        outbox.enqueue(QStringLiteral("t/alarm"), QByteArray("A"), false, 1,
                       MqttOutbox::Alarm);
        const auto afterAlarm = outbox.peek(10);
        P1_CHECK("P1M-15: 高优先级消息不会被低优先级淘汰", afterAlarm.size() == 3
                 && afterAlarm.first().priority == MqttOutbox::Alarm);
        P1_CHECK("P1M-16: 淘汰计数递增", outbox.droppedCount() >= 3);
    }

    {
        MqttOutbox outbox(dir.path() + QStringLiteral("/limit_bytes.db"));
        outbox.setLimits(100, 1024);
        outbox.enqueue(QStringLiteral("t/1"), QByteArray(700, 'a'), false, 0,
                       MqttOutbox::Telemetry);
        outbox.enqueue(QStringLiteral("t/2"), QByteArray(700, 'b'), false, 0,
                       MqttOutbox::Telemetry);
        P1_CHECK("P1M-17: 超过字节上限时淘汰最旧消息", outbox.count() == 1);
        P1_CHECK("P1M-18: 保留最新消息", outbox.peek(1).first().topic == QStringLiteral("t/2"));
    }
}

// ---------------- 进程内迷你 broker ----------------

class P1FakeBroker : public QObject
{
public:
    explicit P1FakeBroker(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections()) {
                QTcpSocket *socket = m_server.nextPendingConnection();
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    buffers[socket].append(socket->readAll());
                    processBuffer(socket);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return m_server.serverPort(); }

    void dropClients()
    {
        const QList<QTcpSocket *> clients = findChildren<QTcpSocket *>();
        for (QTcpSocket *socket : clients)
            socket->abort();
    }

    bool ackPublishes = true;
    int connackCount = 0;
    QVector<QPair<QString, QByteArray>> publishes;

private:
    static QByteArray encodeString(const QString &text)
    {
        const QByteArray utf8 = text.toUtf8();
        QByteArray out;
        out.append(static_cast<char>((utf8.size() >> 8) & 0xFF));
        out.append(static_cast<char>(utf8.size() & 0xFF));
        out.append(utf8);
        return out;
    }

    void processBuffer(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers[socket];
        while (true) {
            if (buffer.size() < 2)
                return;
            int remaining = 0;
            int multiplier = 1;
            int headerSize = 1;
            bool complete = false;
            for (int i = 1; i < buffer.size() && i <= 4; ++i) {
                const unsigned char byte = static_cast<unsigned char>(buffer.at(i));
                remaining += (byte & 0x7F) * multiplier;
                multiplier *= 128;
                headerSize = i + 1;
                if ((byte & 0x80) == 0) {
                    complete = true;
                    break;
                }
            }
            if (!complete || buffer.size() < headerSize + remaining)
                return;

            const unsigned char typeFlags = static_cast<unsigned char>(buffer.at(0));
            const int type = typeFlags >> 4;
            const QByteArray body = buffer.mid(headerSize, remaining);
            buffer.remove(0, headerSize + remaining);

            switch (type) {
            case 1:   // CONNECT
                ++connackCount;
                socket->write(QByteArray("\x20\x02\x00\x00", 4));
                break;
            case 3: { // PUBLISH
                if (body.size() < 2)
                    break;
                const int topicLen = (static_cast<unsigned char>(body.at(0)) << 8)
                                     | static_cast<unsigned char>(body.at(1));
                if (body.size() < 2 + topicLen)
                    break;
                const QString topic = QString::fromUtf8(body.mid(2, topicLen));
                int offset = 2 + topicLen;
                const int qos = (typeFlags >> 1) & 0x03;
                if (qos >= 1) {
                    if (body.size() < offset + 2)
                        break;
                    const int packetId = (static_cast<unsigned char>(body.at(offset)) << 8)
                                         | static_cast<unsigned char>(body.at(offset + 1));
                    offset += 2;
                    if (ackPublishes) {
                        QByteArray ack;
                        ack.append(static_cast<char>(0x40));
                        ack.append(static_cast<char>(0x02));
                        ack.append(static_cast<char>((packetId >> 8) & 0xFF));
                        ack.append(static_cast<char>(packetId & 0xFF));
                        socket->write(ack);
                    }
                }
                publishes.append(qMakePair(topic, body.mid(offset)));
                break;
            }
            case 12:  // PINGREQ
                socket->write(QByteArray("\xD0\x00", 2));
                break;
            case 14:  // DISCONNECT
                socket->disconnectFromHost();
                break;
            default:
                break;
            }
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> buffers;
};

static void testMqttClientOfflineReplay()
{
    printf("\n=== P1 MQTT 离线入队 / 重连补发 ===\n");
    P1FakeBroker broker;
    P1_CHECK("P1M-19: fake broker 启动", broker.start());

    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/client_outbox.db");
    {
        MqttClient client;
        P1_CHECK("P1M-20: 启用客户端 outbox", client.enableOutbox(path));
        client.setPublishQos(1);

        int queuedSignals = 0;
        int drainedSignals = 0;
        QObject::connect(&client, &MqttClient::messageQueued,
                         [&](const QString &, int, int) { ++queuedSignals; });
        QObject::connect(&client, &MqttClient::outboxDrained,
                         [&]() { ++drainedSignals; });

        P1_CHECK("P1M-21: 断线遥测入队", client.publish(QStringLiteral("t/telemetry"),
                                                         QByteArray("T"), false,
                                                         MqttOutbox::Telemetry));
        P1_CHECK("P1M-22: 断线状态入队", client.publish(QStringLiteral("t/status"),
                                                         QByteArray("S"), true,
                                                         MqttOutbox::Status));
        P1_CHECK("P1M-23: 断线报警入队", client.publish(QStringLiteral("t/alarm"),
                                                         QByteArray("A"), false,
                                                         MqttOutbox::Alarm));
        P1_CHECK("P1M-24: 三条消息都在 outbox", client.outboxCount() == 3);
        P1_CHECK("P1M-25: 入队信号触发", queuedSignals == 3);

        client.setBroker(QStringLiteral("127.0.0.1"), broker.port());
        client.setCredentials(QStringLiteral("p1-replay"));
        client.connectToBroker();

        P1_CHECK("P1M-26: 重连后自动补发完", waitUntil([&]() {
            return client.outboxCount() == 0 && broker.publishes.size() == 3;
        }, 4000));
        P1_CHECK("P1M-27: 补发顺序为报警优先", broker.publishes.size() == 3
                 && broker.publishes[0].first == QStringLiteral("t/alarm")
                 && broker.publishes[1].first == QStringLiteral("t/status")
                 && broker.publishes[2].first == QStringLiteral("t/telemetry"));
        P1_CHECK("P1M-28: 报警载荷正确",
                 broker.publishes.size() == 3 && broker.publishes[0].second == QByteArray("A"));
        P1_CHECK("P1M-29: 补发完成信号", drainedSignals >= 1);

        client.disconnectFromBroker();
        waitMs(100);
    }
}

static void testMqttClientOutboxRestart()
{
    printf("\n=== P1 MQTT 离线队列：客户端重启后补发 ===\n");
    P1FakeBroker broker;
    broker.start();
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/restart_outbox.db");

    {
        MqttClient first;
        first.enableOutbox(path);
        first.setPublishQos(1);
        P1_CHECK("P1M-30: 首次运行离线入队", first.publish(QStringLiteral("t/restart"),
                                                            QByteArray("R"), false,
                                                            MqttOutbox::Telemetry));
    }
    {
        MqttClient second;
        P1_CHECK("P1M-31: 重启后队列仍在", second.enableOutbox(path)
                 && second.outboxCount() == 1);
        second.setPublishQos(1);
        second.setBroker(QStringLiteral("127.0.0.1"), broker.port());
        second.setCredentials(QStringLiteral("p1-restart"));
        second.connectToBroker();
        P1_CHECK("P1M-32: 重启后自动补发", waitUntil([&]() {
            return second.outboxCount() == 0 && broker.publishes.size() == 1;
        }, 4000));
        P1_CHECK("P1M-33: 重启补发内容正确", broker.publishes.size() == 1
                 && broker.publishes.first().second == QByteArray("R"));
        second.disconnectFromBroker();
        waitMs(100);
    }
}

static void testMqttClientInflightRequeue()
{
    printf("\n=== P1 MQTT QoS1 在途消息断线重入队 ===\n");
    P1FakeBroker broker;
    broker.ackPublishes = false;
    P1_CHECK("P1M-34: fake broker 启动", broker.start());

    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/inflight_outbox.db");
    MqttClient client;
    P1_CHECK("P1M-35: 启用 outbox", client.enableOutbox(path));
    client.setPublishQos(1);
    client.setBroker(QStringLiteral("127.0.0.1"), broker.port());
    client.setCredentials(QStringLiteral("p1-inflight"));
    client.connectToBroker();
    P1_CHECK("P1M-36: 已连接", waitUntil([&]() {
        return client.isConnectedToBroker();
    }, 2000));

    P1_CHECK("P1M-37: QoS1 直接发布成功",
             client.publish(QStringLiteral("t/inflight"), QByteArray("I"), false,
                            MqttOutbox::Telemetry));
    P1_CHECK("P1M-38: broker 已收到（但不回 PUBACK）", waitUntil([&]() {
        return broker.publishes.size() == 1;
    }, 2000));
    P1_CHECK("P1M-39: 直接发送时未提前入队", client.outboxCount() == 0);

    broker.dropClients();
    P1_CHECK("P1M-40: 断线后在途消息重入队", waitUntil([&]() {
        return client.outboxCount() == 1;
    }, 3000));

    broker.ackPublishes = true;
    client.connectToBroker();
    P1_CHECK("P1M-41: 重连后补发并清空队列", waitUntil([&]() {
        return broker.publishes.size() >= 2 && client.outboxCount() == 0;
    }, 5000));
    client.disconnectFromBroker();
    waitMs(100);
}

static void testMqttClientQueueDisabledAndLimits()
{
    printf("\n=== P1 MQTT 离线队列：关闭行为 / 客户端容量 ===\n");
    QTemporaryDir dir;

    {
        MqttClient client;
        P1_CHECK("P1M-42: 未启用队列时断线发布返回 false",
                 !client.publish(QStringLiteral("t/drop"), QByteArray("D")));
        P1_CHECK("P1M-43: 未启用队列时计数为 0", client.outboxCount() == 0);
    }

    {
        MqttClient client;
        client.enableOutbox(dir.path() + QStringLiteral("/client_limits.db"));
        client.setOutboxLimits(2, 1024 * 1024);
        client.publish(QStringLiteral("t/1"), QByteArray("1"), false, MqttOutbox::Telemetry);
        client.publish(QStringLiteral("t/2"), QByteArray("2"), false, MqttOutbox::Telemetry);
        client.publish(QStringLiteral("t/3"), QByteArray("3"), false, MqttOutbox::Telemetry);
        P1_CHECK("P1M-44: 客户端容量上限生效", client.outboxCount() == 2);
        P1_CHECK("P1M-45: 淘汰计数可查询", client.outboxDroppedCount() >= 1);
        P1_CHECK("P1M-46: 清空 outbox", client.clearOutbox() && client.outboxCount() == 0);
        client.disableOutbox();
        P1_CHECK("P1M-47: 关闭 outbox 后计数为 0", client.outboxCount() == 0);
    }
}

void runP1AlarmMqttTests()
{
    printf("\n########## P1 报警持久化 / MQTT 离线队列 ##########\n");
    testAlarmPersistenceBasic();
    testAlarmStateRestore();
    testAlarmDeleteAndClear();
    testAlarmRepositoryFailure();
    testMqttOutboxBasics();
    testMqttOutboxLimits();
    testMqttClientOfflineReplay();
    testMqttClientOutboxRestart();
    testMqttClientInflightRequeue();
    testMqttClientQueueDisabledAndLimits();
}

int p1AlarmMqttPassed()
{
    return g_p1Passed;
}

int p1AlarmMqttFailed()
{
    return g_p1Failed;
}
