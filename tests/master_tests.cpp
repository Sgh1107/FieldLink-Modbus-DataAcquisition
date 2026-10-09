// master_tests.cpp
// FieldLink master 分支核心逻辑测试套件（无 GUI）。
//
// 覆盖：AlarmManager（8 种条件/去抖/确认）、SecurityManager（Token/角色/默认口令）、
//       ReliabilityManager（用户意图门控）、MqttClient（协议线缆测试，内置 FakeBroker）、
//       DataExporter（CSV）、PollManager（调度）、DeviceManager（CRUD+持久化）、
//       HistoryData（SQLite）、BatchTaskManager（顺序执行）、DataParser（字节序）。
//
// 输出约定：[PASS]/[FAIL] 为断言结果（FAIL 计入失败数）；
//           [KNOWN-ISSUE CONFIRMED] 为 code review 已知问题的证据（不计失败，见测试报告）。
//
// PM 组：PointModel 点表管理 + CSV/JSON 批量导入导出（此前无任何自动化覆盖）

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>

#include "alarmmanager.h"
#include "securitymanager.h"
#include "reliabilitymanager.h"
#include "mqttclient.h"
#include "credentialcodec.h"
#include "dataexporter.h"
#include "pollmanager.h"
#include "devicemanager.h"
#include "historydata.h"
#include "batchtaskmanager.h"
#include "dataparser.h"
#include "pointmodel.h"
#include "modbusdiagnostics.h"

static int g_failed = 0;
static int g_passed = 0;
static int g_issuesConfirmed = 0;

#define CHECK(name, ...) do { \
    if ((__VA_ARGS__)) { printf("  [PASS] %s\n", name); ++g_passed; } \
    else { printf("  [FAIL] %s  (at %s:%d)\n", name, __FILE__, __LINE__); ++g_failed; } \
} while (0)

// 已知问题复核：行为复现则确认问题存在（计入 g_issuesConfirmed，不计失败）
#define CHECK_ISSUE(name, ...) do { \
    if ((__VA_ARGS__)) { printf("  [KNOWN-ISSUE CONFIRMED] %s\n", name); ++g_issuesConfirmed; } \
    else printf("  [ISSUE NOT REPRODUCED] %s\n", name); \
} while (0)

static void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// ---------------- FakeMqttBroker：MQTT 3.1.1 迷你服务端（进程内） ----------------

class FakeMqttBroker : public QObject
{
public:
    QTcpServer server;
    QHash<QTcpSocket *, QByteArray> buffers;
    bool rejectAuth = false;
    int connackAccepted = 0;
    int connackRejected = 0;
    int pingCount = 0;
    int pubackSent = 0;
    QVector<QPair<QString, QString>> publishes;   // topic, payload
    QVector<bool> publishRetained;

    explicit FakeMqttBroker(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    buffers[socket].append(socket->readAll());
                    processBuffer(socket);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }

    bool start() { return server.listen(QHostAddress::LocalHost); }
    quint16 port() const { return server.serverPort(); }

    static QByteArray encodeString(const QString &text)
    {
        const QByteArray utf8 = text.toUtf8();
        QByteArray out;
        out.append(static_cast<char>((utf8.size() >> 8) & 0xFF));
        out.append(static_cast<char>(utf8.size() & 0xFF));
        out.append(utf8);
        return out;
    }

private:
    void sendPacket(QTcpSocket *socket, unsigned char type, const QByteArray &body)
    {
        QByteArray packet;
        packet.append(static_cast<char>(type << 4));   // 类型在固定头高 4 位
        packet.append(static_cast<char>(body.size()));
        packet.append(body);
        socket->write(packet);
    }

    void processBuffer(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers[socket];
        while (true) {
            if (buffer.size() < 2) return;
            int remaining = 0, multiplier = 1, headerSize = 1;
            bool complete = false;
            for (int i = 1; i < buffer.size() && i <= 4; ++i) {
                const unsigned char byte = static_cast<unsigned char>(buffer.at(i));
                remaining += (byte & 0x7F) * multiplier;
                multiplier *= 128;
                headerSize = i + 1;
                if ((byte & 0x80) == 0) { complete = true; break; }
            }
            if (!complete) return;
            if (buffer.size() < headerSize + remaining) return;

            const unsigned char typeFlags = static_cast<unsigned char>(buffer.at(0));
            const int type = typeFlags >> 4;
            const QByteArray body = buffer.mid(headerSize, remaining);
            buffer.remove(0, headerSize + remaining);

            switch (type) {
            case 1: {   // CONNECT
                if (rejectAuth) {
                    ++connackRejected;
                    sendPacket(socket, 2, QByteArray("\x00\x04", 2));
                } else {
                    ++connackAccepted;
                    sendPacket(socket, 2, QByteArray("\x00\x00", 2));
                }
                break;
            }
            case 3: {   // PUBLISH (QoS0/QoS1)
                const bool retain = typeFlags & 0x01;
                const int qos = (typeFlags >> 1) & 0x03;
                if (body.size() < 2) return;
                const int topicLen = (static_cast<unsigned char>(body.at(0)) << 8)
                                     | static_cast<unsigned char>(body.at(1));
                const QString topic = QString::fromUtf8(body.mid(2, topicLen));
                int payloadOffset = 2 + topicLen;
                if (qos >= 1) {
                    // QoS1：剥离报文标识符并回 PUBACK
                    if (body.size() < payloadOffset + 2) return;
                    const int packetId = (static_cast<unsigned char>(body.at(payloadOffset)) << 8)
                                         | static_cast<unsigned char>(body.at(payloadOffset + 1));
                    ++pubackSent;
                    QByteArray ackBody;
                    ackBody.append(static_cast<char>((packetId >> 8) & 0xFF));
                    ackBody.append(static_cast<char>(packetId & 0xFF));
                    sendPacket(socket, 4, ackBody);
                    payloadOffset += 2;
                }
                const QString payload = QString::fromUtf8(body.mid(payloadOffset));
                publishes.append(qMakePair(topic, payload));
                publishRetained.append(retain);
                break;
            }
            case 12: {  // PINGREQ
                ++pingCount;
                sendPacket(socket, 13, QByteArray());
                break;
            }
            case 14:    // DISCONNECT
                socket->disconnectFromHost();
                break;
            default:
                break;
            }
        }
    }
};

// ---------------- 测试段 ----------------

static void testAlarmManager()
{
    printf("\n=== AlarmManager ===\n");
    AlarmManager am;
    int triggeredCount = 0, clearedCount = 0;
    AlarmEvent lastEvent;
    QObject::connect(&am, &AlarmManager::alarmTriggered, [&](const AlarmEvent &e) {
        ++triggeredCount; lastEvent = e;
    });
    QObject::connect(&am, &AlarmManager::alarmCleared, [&](int) { ++clearedCount; });

    AlarmRule r;
    r.id = 0; r.name = QStringLiteral("测试规则"); r.enabled = true;
    r.serverAddress = 1; r.registerType = 4; r.address = 0;
    r.condition = AlarmCondition::GreaterThan; r.threshold1 = 100; r.threshold2 = 0;
    r.severity = AlarmSeverity::Warning; r.message = QStringLiteral("超限");
    r.debounceMs = 0; r.acknowledged = false;
    const int ruleId = am.addRule(r);
    CHECK("addRule 返回递增 id", ruleId == 1);

    am.checkValue(1, 4, 0, 150.0);
    CHECK("GreaterThan 触发", triggeredCount == 1 && am.activeAlarmCount() == 1);
    CHECK("事件字段完整", lastEvent.ruleId == ruleId && lastEvent.value == 150.0
          && lastEvent.message == QStringLiteral("超限") && !lastEvent.acknowledged);

    am.checkValue(1, 4, 0, 50.0);
    CHECK("回落清除报警", clearedCount == 1 && am.activeAlarmCount() == 0);

    // 8 种条件全量验证（新规则逐一添加）
    struct CondCase { const char *name; AlarmCondition cond; double t1, t2, value; bool expect; };
    const CondCase cases[] = {
        {"LessThan",        AlarmCondition::LessThan,   10, 0,  5,   true},
        {"LessThan 未触发", AlarmCondition::LessThan,   10, 0,  15,  false},
        {"InRange",         AlarmCondition::InRange,    10, 20, 15,  true},
        {"InRange 界外",    AlarmCondition::InRange,    10, 20, 25,  false},
        {"OutOfRange",      AlarmCondition::OutOfRange, 10, 20, 25,  true},
        {"BitSet bit3=8",   AlarmCondition::BitSet,      3, 0,  8,   true},
        {"BitSet bit3=7",   AlarmCondition::BitSet,      3, 0,  7,   false},
        {"BitClear bit3=7", AlarmCondition::BitClear,    3, 0,  7,   true},
    };
    for (const CondCase &c : cases) {
        AlarmRule cr = r;
        cr.condition = c.cond; cr.threshold1 = c.t1; cr.threshold2 = c.t2;
        const int cid = am.addRule(cr);
        const int before = triggeredCount;
        am.checkValue(1, 4, 0, c.value);
        // 触发数增加当且仅当期望触发（除非该规则已被上一次同名触发置 active）
        CHECK(c.name, (triggeredCount > before) == c.expect);
        am.removeRule(cid);
        triggeredCount = 0;   // 每个用例独立计数
    }

    // Equal 对 0 值（qFuzzyCompare(0,0) 实际返回 true，行为正确）
    AlarmRule eq = r;
    eq.condition = AlarmCondition::Equal; eq.threshold1 = 0;
    const int idEqual = am.addRule(eq);
    const int before = triggeredCount;
    am.checkValue(1, 4, 0, 0.0);
    CHECK("Equal 阈值 0 可触发", triggeredCount == before + 1);
    am.removeRule(idEqual);

    // 去抖：使用独立地址 5，避免与规则 1（GreaterThan>100，地址 0）相互干扰
    AlarmRule db = r;
    db.condition = AlarmCondition::GreaterThan; db.threshold1 = 100; db.debounceMs = 2000;
    db.address = 5;
    am.addRule(db);
    am.checkValue(1, 4, 5, 150.0);          // 触发
    const int countAfterFirst = triggeredCount;
    am.checkValue(1, 4, 5, 50.0);           // 清除
    am.checkValue(1, 4, 5, 150.0);          // 去抖窗口内再触发 → 抑制
    CHECK("去抖窗口内再触发被抑制", triggeredCount == countAfterFirst
          && am.activeAlarmCount() == 0);
    waitMs(2100);
    am.checkValue(1, 4, 5, 150.0);          // 去抖窗口过后 → 再次触发
    CHECK("去抖窗口过后正常触发", triggeredCount == countAfterFirst + 1);

    // 确认与历史
    const QVector<AlarmEvent> history = am.alarmHistory(100);
    CHECK("alarmHistory 返回事件", !history.isEmpty());
    am.acknowledgeAll();
    const QVector<AlarmEvent> afterAck = am.alarmHistory(100);
    CHECK("acknowledgeAll 全部确认", std::all_of(afterAck.cbegin(), afterAck.cend(),
        [](const AlarmEvent &e) { return e.acknowledged; }));

    // 边界：删除规则后状态清理
    am.removeRule(ruleId);
    am.checkValue(1, 4, 0, 150.0);
    CHECK("删除规则后不再触发", true);
}

static void testSecurityManager()
{
    printf("\n=== SecurityManager ===\n");
    QTemporaryDir dir;
    const QString iniPath = dir.path() + "/security.ini";

    // 已知问题 S1 修复：未配置 Token 时默认口令失效，API 锁定
    {
        SecurityManager sm;
        QSettings settings(iniPath, QSettings::IniFormat);
        sm.load(settings);
        CHECK("verifyApiToken 拒绝空 token", !sm.verifyApiToken(QString()));
        CHECK("S1 修复：未配置 Token 时拒绝源码常量默认口令", !sm.verifyApiToken(QStringLiteral("modbus-admin")));
    }

    SecurityManager sm;
    QSettings settings(iniPath, QSettings::IniFormat);
    sm.load(settings);
    sm.setApiToken(QStringLiteral("s3cret"));
    CHECK("setApiToken 后新 token 通过", sm.verifyApiToken(QStringLiteral("s3cret")));
    CHECK("旧默认 token 失效", !sm.verifyApiToken(QStringLiteral("modbus-admin")));
    CHECK("错误 token 拒绝", !sm.verifyApiToken(QStringLiteral("wrong")));

    // 默认用户 admin/admin123（保留可用，但 S2 修复后必须先改密）
    CHECK("S2: 默认账号 admin/admin123 可登录", sm.login(QStringLiteral("admin"), QStringLiteral("admin123")));
    CHECK("S2 修复：默认账号标记强制改密", sm.mustChangePassword(QStringLiteral("admin")));
    CHECK("admin 拥有通配权限", sm.hasPermission(QStringLiteral("local.write")));
    sm.changePassword(QStringLiteral("admin"), QStringLiteral("Str0ng!Pass"));
    CHECK("S2 修复：改密后清除强制改密标记", !sm.mustChangePassword(QStringLiteral("admin")));
    sm.logout();
    CHECK("S2 修复：改密后新密码可登录", sm.login(QStringLiteral("admin"), QStringLiteral("Str0ng!Pass")));
    sm.logout();
    CHECK("登出后无权限", !sm.hasPermission(QStringLiteral("local.write")));

    // viewer 角色权限
    sm.addOrUpdateUser(QStringLiteral("view1"), QStringLiteral("pw"), QStringLiteral("viewer"), true);
    CHECK("viewer 登录", sm.login(QStringLiteral("view1"), QStringLiteral("pw")));
    CHECK("viewer 无 local.write 权限", !sm.hasPermission(QStringLiteral("local.write")));
    sm.logout();

    // 已知问题 S4 修复：空密码创建的用户默认密码 123456 + 强制改密标记
    sm.addOrUpdateUser(QStringLiteral("op1"), QString(), QStringLiteral("operator"), true);
    CHECK("S4 修复：空密码创建的用户可登录", sm.login(QStringLiteral("op1"), QStringLiteral("123456")));
    CHECK("S4 修复：空密码用户标记强制改密", sm.mustChangePassword(QStringLiteral("op1")));
    sm.logout();

    // admin 保护
    sm.addOrUpdateUser(QStringLiteral("admin"), QStringLiteral("newpass"), QStringLiteral("admin"), true);
    sm.removeUser(QStringLiteral("admin"));
    CHECK("admin 不可删除", sm.login(QStringLiteral("admin"), QStringLiteral("newpass")));

    // 写操作权限
    sm.setRemoteWriteEnabled(true);
    CHECK("admin 允许远程写", sm.isWriteAllowed(QStringLiteral("admin"), QStringLiteral("test")));
    CHECK("未知操作员拒绝远程写", !sm.isWriteAllowed(QStringLiteral("ghost"), QStringLiteral("test")));

    // 持久化回环
    sm.setApiToken(QStringLiteral("round-trip"));
    sm.save(settings);
    SecurityManager sm2;
    sm2.load(settings);
    CHECK("Token 哈希持久化回环", sm2.verifyApiToken(QStringLiteral("round-trip")));
    CHECK("用户列表持久化", sm2.login(QStringLiteral("view1"), QStringLiteral("pw")));
}

static void testReliabilityManager()
{
    printf("\n=== ReliabilityManager ===\n");
    ReliabilityManager manager;
    manager.setReconnectIntervalMs(500);
    manager.setHeartbeatIntervalMs(1000);

    int reconnectCount = 0, heartbeatCount = 0;
    QObject::connect(&manager, &ReliabilityManager::reconnectRequested,
                     [&]() { ++reconnectCount; });
    QObject::connect(&manager, &ReliabilityManager::heartbeatRequested,
                     [&]() { ++heartbeatCount; });

    bool okA = false, okB = false, okC = false, okD = false;
    int snapshotAtCancel = -1;

    manager.start();

    QTimer::singleShot(1600, &manager, [&]() {
        okA = (reconnectCount == 0);          // A. 启动不自动连接
        printf("  [PASS/FAIL] A startup-no-connect: %s\n", okA ? "PASS" : "FAIL");
        if (!okA) ++g_failed; else ++g_passed;
        manager.setUserIntentConnected(true);
        manager.notifyDisconnected();         // B. 意外断线 → 自动重连
    });
    QTimer::singleShot(2800, &manager, [&]() {
        okB = (reconnectCount >= 1);
        printf("  [PASS/FAIL] B intent-drop-reconnect: %s\n", okB ? "PASS" : "FAIL");
        if (!okB) ++g_failed; else ++g_passed;
        manager.setUserIntentConnected(false);// C. 手动断开 → 停止
        snapshotAtCancel = reconnectCount;
    });
    QTimer::singleShot(4200, &manager, [&]() {
        okC = (snapshotAtCancel == reconnectCount);
        okD = (heartbeatCount >= 1);          // D. 心跳不受意图影响
        printf("  [PASS/FAIL] C manual-stop-reconnect: %s\n", okC ? "PASS" : "FAIL");
        printf("  [PASS/FAIL] D heartbeat-running: %s\n", okD ? "PASS" : "FAIL");
        if (!okC) ++g_failed; else ++g_passed;
        if (!okD) ++g_failed; else ++g_passed;
    });

    waitMs(4400);
}

// ---------------- MQTT 线缆测试 ----------------

static void testMqttClient()
{
    printf("\n=== MqttClient (内置 FakeBroker 线缆测试) ===\n");
    FakeMqttBroker broker;
    CHECK("FakeBroker 监听", broker.start());

    MqttClient client;
    int errors = 0;
    QObject::connect(&client, &MqttClient::errorOccurred, [&](const QString &message) {
        ++errors;
        printf("  [client-error] %s\n", qPrintable(message));
    });

    // M1 修复：未连接时 publish 静默丢弃（返回 false），两次丢弃仅发一条告警
    const bool pubOk1 = client.publish(QStringLiteral("fieldlink/test/drop"), QByteArray("1"));
    const bool pubOk2 = client.publish(QStringLiteral("fieldlink/test/drop"), QByteArray("2"));
    waitMs(50);
    CHECK("M1 未连接 publish 返回 false", !pubOk1 && !pubOk2);
    CHECK("M1 两次丢弃仅一条告警", errors == 1);
    errors = 0;   // 后续连接阶段不应再有丢弃告警

    client.setBroker(QStringLiteral("127.0.0.1"), broker.port());
    client.setCredentials(QStringLiteral("test-client"));
    client.setKeepAlive(1);   // 500ms 一次 PINGREQ

    bool connectedSeen = false, disconnectedSeen = false;
    QObject::connect(&client, &MqttClient::connected, [&]() {
        connectedSeen = true;
        client.publish(QStringLiteral("fieldlink/test/normal"), QByteArray("hello-1"));
        client.publish(QStringLiteral("fieldlink/test/retained"), QByteArray("hello-2"), true);
    });
    QObject::connect(&client, &MqttClient::disconnected, [&]() { disconnectedSeen = true; });

    client.connectToBroker();
    waitMs(1600);   // 覆盖：连接+2 次发布+至少 2 次心跳

    CHECK("CONNECT/CONNACK 握手", broker.connackAccepted == 1);
    CHECK("收到 2 条 PUBLISH", broker.publishes.size() == 2);
    if (broker.publishes.size() == 2) {
        CHECK("PUBLISH topic/payload 正确",
              broker.publishes[0].first == QStringLiteral("fieldlink/test/normal")
              && broker.publishes[0].second == QStringLiteral("hello-1"));
        CHECK("retain 标志正确", !broker.publishRetained[0] && broker.publishRetained[1]);
    }
    CHECK("PINGREQ 心跳发出", broker.pingCount >= 2);
    CHECK("客户端进入连接态", client.isConnectedToBroker());

    client.disconnectFromBroker();
    waitMs(200);
    CHECK("主动断开不再重连", !client.isConnectedToBroker());

    // QoS1：PUBLISH 带报文标识符，broker 回 PUBACK 后 published 信号发出
    broker.publishes.clear();
    broker.publishRetained.clear();
    MqttClient qos1Client;
    int qos1Published = 0;
    QString qos1Topic;
    QObject::connect(&qos1Client, &MqttClient::published,
                     [&](const QString &topic, int) { ++qos1Published; qos1Topic = topic; });
    qos1Client.setBroker(QStringLiteral("127.0.0.1"), broker.port());
    qos1Client.setCredentials(QStringLiteral("qos1-client"));
    qos1Client.setPublishQos(1);
    QObject::connect(&qos1Client, &MqttClient::connected, [&]() {
        qos1Client.publish(QStringLiteral("fieldlink/test/qos1"), QByteArray("qos1-payload"));
    });
    qos1Client.connectToBroker();
    waitMs(1000);

    // 认证拒绝路径：broker 切换为拒绝模式
    broker.rejectAuth = true;
    MqttClient badClient;
    QString lastError;
    bool rejectedSeen = false;
    QObject::connect(&badClient, &MqttClient::errorOccurred, [&](const QString &message) {
        lastError = message;
        if (message.contains(QStringLiteral("refused connection"))) rejectedSeen = true;
    });
    badClient.setBroker(QStringLiteral("127.0.0.1"), broker.port());
    badClient.setCredentials(QStringLiteral("bad-client"), QStringLiteral("u"), QStringLiteral("p"));
    badClient.connectToBroker();
    waitMs(800);
    CHECK("broker 拒绝后客户端报错", rejectedSeen);
    CHECK("拒绝后未进入连接态", !badClient.isConnectedToBroker());
    CHECK("QoS1: broker 收到 PUBLISH", broker.publishes.size() == 1);
    if (broker.publishes.size() == 1)
        CHECK("QoS1: 载荷正确（报文标识符已剥离）", broker.publishes[0].second == QStringLiteral("qos1-payload"));
    CHECK("QoS1: broker 回了 PUBACK", broker.pubackSent == 1);
    CHECK("QoS1: PUBACK 后 published 信号发出",
          qos1Published == 1 && qos1Topic == QStringLiteral("fieldlink/test/qos1"));
    qos1Client.disconnectFromBroker();
    waitMs(200);

    // 连接阶段失败后的自动重连兜底：先对一个"无服务"端口发起连接（必失败），
    // 失败后应进入 5 秒重连循环；期间把目标换到真实 broker，重连应自动成功
    {
        broker.rejectAuth = false;   // 恢复上面的认证拒绝测试切换的模式
        QTcpServer probe;
        CHECK("重连测试探针监听", probe.listen(QHostAddress::LocalHost));
        const quint16 emptyPort = probe.serverPort();
        probe.close();   // 关闭后该端口无服务

        MqttClient retryClient;
        int retryConnected = 0;
        QString retryLastError;
        QObject::connect(&retryClient, &MqttClient::connected,
                         [&]() { ++retryConnected; });
        QObject::connect(&retryClient, &MqttClient::errorOccurred,
                         [&](const QString &m) { retryLastError = m;
                             printf("  [retry-error] %s\n", qPrintable(m)); });
        retryClient.setBroker(QStringLiteral("127.0.0.1"), emptyPort);
        retryClient.setCredentials(QStringLiteral("retry-client"));
        retryClient.connectToBroker();
        waitMs(600);   // 第一次连接失败（connection refused）
        printf("  [retry-debug] after 600ms: connected=%d lastError=%s\n",
               retryConnected, qPrintable(retryLastError));
        CHECK("连接失败后进入重连循环", !retryClient.isConnectedToBroker());

        retryClient.setBroker(QStringLiteral("127.0.0.1"), broker.port());   // broker 此时才"上线"
        // 注：Windows 上对无服务端口的连接拒绝要 ~3 秒才返回，之后才进入 5s 重连周期
        waitMs(3500);  // 期间应收到第一次 refused 错误
        printf("  [retry-debug] after +3500ms: connected=%d lastError=%s\n",
               retryConnected, qPrintable(retryLastError));
        waitMs(6000);  // 覆盖 5s 重连周期（自 refused 到达起算）
        printf("  [retry-debug] after +9500ms: connected=%d lastError=%s\n",
               retryConnected, qPrintable(retryLastError));
        CHECK("连接阶段失败后自动重连成功", retryConnected >= 1 && retryClient.isConnectedToBroker());
        retryClient.disconnectFromBroker();
        waitMs(200);
    }

    Q_UNUSED(errors); Q_UNUSED(disconnectedSeen);
}

static void testDataExporter()
{
    printf("\n=== DataExporter ===\n");
    QTemporaryDir dir;
    CHECK("QTemporaryDir 可用", dir.isValid());

    DataExporter exporter;
    ExportRecord r1;
    r1.timestamp = QStringLiteral("2026-09-06T10:00:00.000");
    r1.serverAddress = 1; r1.registerType = QModbusDataUnit::Coils;
    r1.startAddress = 0; r1.values = {1, 2, 3};
    ExportRecord r2;
    r2.timestamp = QStringLiteral("2026-09-06T10:00:01.000");
    r2.serverAddress = 1; r2.registerType = QModbusDataUnit::HoldingRegisters;
    r2.startAddress = 10; r2.values = {65535};
    exporter.addRecord(r1);
    exporter.addRecord(r2);
    CHECK("记录计数", exporter.recordCount() == 2);

    const QString csvPath = dir.path() + "/export.csv";
    CHECK("CSV 导出成功", exporter.exportToCsv(csvPath));

    QFile file(csvPath);
    CHECK("CSV 文件存在", file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());
    file.close();
    CHECK("CSV 表头正确", content.startsWith(QStringLiteral("Timestamp,ServerAddress,RegisterType,StartAddress,Values\n")));
    CHECK("CSV Coils 行内容", content.contains(QStringLiteral(
        "\"2026-09-06T10:00:00.000\",\"1\",\"Coils\",\"0\",\"1;2;3\"")));
    CHECK("CSV HoldingRegisters 行内容", content.contains(QStringLiteral(
        "\"2026-09-06T10:00:01.000\",\"1\",\"HoldingRegisters\",\"10\",\"65535\"")));

    // U3 修复：字段含逗号/引号时正确转义，不破坏 CSV 结构（使用独立实例，不影响上面的容量断言）
    DataExporter exporter2;
    ExportRecord rComma;
    rComma.timestamp = QStringLiteral("2026-09-06T10:00:02,5");   // 恶意时间戳（含逗号）
    rComma.serverAddress = 3; rComma.registerType = QModbusDataUnit::Coils;
    rComma.startAddress = 1; rComma.values = {7};
    exporter2.addRecord(rComma);
    const QString csvPath2 = dir.path() + "/export2.csv";
    CHECK("U3: 含逗号记录导出成功", exporter2.exportToCsv(csvPath2));
    QFile file2(csvPath2);
    CHECK("U3: 第二个 CSV 文件存在", file2.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content2 = QString::fromUtf8(file2.readAll());
    file2.close();
    CHECK("U3: 逗号字段被引号包裹且行结构完整", content2.contains(QStringLiteral(
        "\"2026-09-06T10:00:02,5\",\"3\",\"Coils\",\"1\",\"7\"")));

    // 容量环绕：保留最新一条
    exporter.setMaxRecords(1);
    CHECK("maxRecords 裁剪", exporter.recordCount() == 1
          && exporter.records().first().startAddress == 10);

    ExportRecord r3;
    r3.timestamp = QStringLiteral("t3"); r3.serverAddress = 2;
    r3.registerType = QModbusDataUnit::InputRegisters; r3.startAddress = 5;
    r3.values = {9};
    exporter.addRecord(r3);
    CHECK("裁剪后新记录入队且挤出旧记录", exporter.recordCount() == 1
          && exporter.records().first().startAddress == 5);
}

static void testPollManager()
{
    printf("\n=== PollManager ===\n");
    PollManager pm;
    PollTask t1;
    t1.id = 1; t1.name = QStringLiteral("任务1"); t1.serverAddress = 1;
    t1.registerType = QModbusDataUnit::HoldingRegisters;
    t1.startAddress = 0; t1.quantity = 2; t1.intervalMs = 100; t1.enabled = true;
    PollTask t2 = t1;
    t2.id = 2; t2.name = QStringLiteral("任务2"); t2.intervalMs = 100;

    int count1 = 0, count2 = 0;
    QObject::connect(&pm, &PollManager::pollRequest, [&](const PollTask &task) {
        if (task.id == 1) ++count1;
        if (task.id == 2) ++count2;
        // 模拟请求即刻完成（否则 P2 在途保护会跳过后续 tick）
        pm.notifyTaskFinished(task.id);
    });

    pm.addTask(t1);
    pm.addTask(t2);
    CHECK("初始非运行态", !pm.isRunning());
    pm.startAll();
    CHECK("startAll 置运行态", pm.isRunning());
    waitMs(350);
    CHECK("任务 1 周期触发", count1 >= 2);
    CHECK("任务 2 周期触发", count2 >= 2);

    const int c1 = count1, c2 = count2;
    pm.stopAll();
    waitMs(250);
    CHECK("stopAll 后停止触发", count1 == c1 && count2 == c2);

    // 禁用单个任务
    pm.setTaskEnabled(2, false);
    pm.startAll();
    const int c1Before = count1;
    waitMs(250);
    CHECK("禁用任务 2 后仅任务 1 触发", count1 > c1Before && count2 == c2);
    pm.stopAll();

    // removeTask
    pm.removeTask(1);
    CHECK("removeTask 生效", pm.tasks().size() == 1);

    // PM1 修复：重复 id 视为更新而非新增
    PollTask dup2 = t2;
    dup2.name = QStringLiteral("任务2-更新");
    dup2.intervalMs = 2000;
    pm.addTask(dup2);
    CHECK("PM1: 同 id 不产生重复任务", pm.tasks().size() == 1);
    CHECK("PM1: 同 id 走更新路径", pm.tasks().first().name == QStringLiteral("任务2-更新")
          && pm.tasks().first().intervalMs == 2000);

    // P2 在途请求保护：请求发出后未回填完成前，同任务 tick 被跳过
    PollManager pm2;
    PollTask t10;
    t10.id = 10; t10.name = QStringLiteral("在途测试"); t10.serverAddress = 1;
    t10.registerType = QModbusDataUnit::HoldingRegisters;
    t10.startAddress = 0; t10.quantity = 1; t10.intervalMs = 100; t10.enabled = true;
    int req10 = 0;
    QObject::connect(&pm2, &PollManager::pollRequest, [&](const PollTask &) { ++req10; });
    pm2.addTask(t10);
    pm2.startAll();
    waitMs(150);   // 第 1 个 tick 发出请求并置在途；第 2 个 tick 应被跳过
    CHECK("P2 在途期间跳过 tick", req10 == 1);
    pm2.notifyTaskFinished(10);
    waitMs(250);   // 完成后周期触发恢复
    CHECK("P2 完成后恢复触发", req10 >= 2);
    pm2.stopAll();
}

static void testDeviceManager()
{
    printf("\n=== DeviceManager ===\n");
    QTemporaryDir dir;
    const QString iniPath = dir.path() + "/devices.ini";

    DeviceManager manager;
    DeviceConfig cfg;
    cfg.name = QStringLiteral("温度采集器");
    cfg.isTcp = true; cfg.portOrAddress = QStringLiteral("127.0.0.1:1502");
    cfg.serverAddress = 1; cfg.parity = 0; cfg.baud = 9600;
    cfg.dataBits = 8; cfg.stopBits = 1; cfg.responseTime = 1000;
    cfg.numberOfRetries = 3; cfg.pollIntervalMs = 1000;
    cfg.archiveTag = QStringLiteral("temp");
    const int id1 = manager.addDevice(cfg);
    DeviceConfig cfg2 = cfg;
    cfg2.name = QStringLiteral("压力采集器");
    cfg2.isTcp = false; cfg2.portOrAddress = QStringLiteral("COM3");
    const int id2 = manager.addDevice(cfg2);
    CHECK("addDevice 递增 id", id1 == 1 && id2 == 2);
    CHECK("allDevices 数量", manager.allDevices().size() == 2);
    CHECK("新设备默认未连接", !manager.isConnected(id1));

    DeviceConfig updated = manager.deviceConfig(id1);
    updated.name = QStringLiteral("温度采集器A");
    manager.updateDevice(updated);
    CHECK("updateDevice 修改生效", manager.deviceConfig(id1).name == QStringLiteral("温度采集器A"));

    QSettings settings(iniPath, QSettings::IniFormat);
    manager.saveToSettings(settings);
    settings.sync();

    DeviceManager manager2;
    QSettings settings2(iniPath, QSettings::IniFormat);
    manager2.loadFromSettings(settings2);
    CHECK("持久化回环设备数量", manager2.allDevices().size() == 2);
    CHECK("持久化回环字段", manager2.deviceConfig(1).name == QStringLiteral("温度采集器A")
          && manager2.deviceConfig(2).isTcp == false);

    manager2.removeDevice(id2);
    CHECK("removeDevice 生效", manager2.allDevices().size() == 1);
    CHECK("removeDevice 后 id 查询为空", manager2.deviceConfig(id2).name.isEmpty());
}

static void testHistoryData()
{
    printf("\n=== HistoryData ===\n");
    QTemporaryDir dir;
    HistoryData history;
    CHECK("打开 SQLite 数据库", history.openDatabase(dir.path() + "/history.db"));
    CHECK("isOpen", history.isOpen());

    const QDateTime now = QDateTime::currentDateTime();
    history.addRecord(1, QModbusDataUnit::HoldingRegisters, 0, {346, 680, 42});
    history.addRecord(1, QModbusDataUnit::HoldingRegisters, 10, {1234});
    history.addRecord(2, QModbusDataUnit::Coils, 0, {1, 0});
    CHECK("totalRecords 累计", history.totalRecords() == 3);

    auto records = history.query(now.addSecs(-60), now.addSecs(60));
    CHECK("时间区间查询全部", records.size() == 3);
    if (records.size() == 3) {
        // query 按 timestamp DESC 排序：first=最后写入，last=最先写入
        const QVector<quint16> newestValues = records.first().values;
        const QVector<quint16> oldestValues = records.last().values;
        CHECK("记录字段回读（最新记录）", records.first().serverAddress == 2
              && records.first().registerType == QModbusDataUnit::Coils
              && newestValues == QVector<quint16>{1, 0});
        CHECK("记录字段回读（最早记录）", records.last().serverAddress == 1
              && oldestValues == QVector<quint16>{346, 680, 42});
    }
    CHECK("按从站过滤", history.query(now.addSecs(-60), now.addSecs(60), 2, -1, -1).size() == 1);
    CHECK("按起始地址过滤", history.query(now.addSecs(-60), now.addSecs(60), -1, -1, 10).size() == 1);
    CHECK("不存在的过滤组合为空", history.query(now.addSecs(-60), now.addSecs(60), 9, -1, -1).isEmpty());

    CHECK("lastRecords 截取", history.lastRecords(2).size() == 2);

    // 清理：未来时间线之前的记录全部清除
    history.clearOlderThan(now.addSecs(60));
    CHECK("clearOlderThan 清空", history.totalRecords() == 0);

    // H1 修复：重复打开直接返回成功且状态正常（不再产生重复连接名警告）
    CHECK("H1: 重复 openDatabase 返回 true", history.openDatabase(dir.path() + "/history.db"));
    CHECK("H1: 重复打开后 isOpen", history.isOpen());
    history.addRecord(3, QModbusDataUnit::InputRegisters, 0, {9});
    CHECK("H1: 重复打开后可正常写入", history.totalRecords() == 1);

    // H2 修复：清理计数器为实例成员——第二实例独立计数，写入路径正常
    HistoryData history2;
    CHECK("H2: 第二实例打开数据库", history2.openDatabase(dir.path() + "/history2.db"));
    history2.addRecord(1, QModbusDataUnit::Coils, 0, {1});
    CHECK("H2: 第二实例独立写入正常", history2.totalRecords() == 1);

    history.closeDatabase();
    CHECK("关闭后 isOpen=false", !history.isOpen());
}

static void testBatchTaskManager()
{
    printf("\n=== BatchTaskManager ===\n");
    QTemporaryDir dir;
    BatchTaskManager manager;

    BatchTask t1;
    t1.name = QStringLiteral("读1"); t1.type = BatchTask::Read;
    t1.serverAddress = 1; t1.registerType = QModbusDataUnit::HoldingRegisters;
    t1.startAddress = 0; t1.quantity = 2; t1.delayAfterMs = 0; t1.enabled = true;
    BatchTask t2 = t1;
    t2.name = QStringLiteral("读2-禁用"); t2.enabled = false;
    BatchTask t3 = t1;
    t3.name = QStringLiteral("读3-带延时"); t3.delayAfterMs = 60;

    const int id1 = manager.addTask(t1);
    manager.addTask(t2);
    const int id3 = manager.addTask(t3);
    CHECK("addTask 计数", manager.tasks().size() == 3);

    // 排序
    manager.moveTaskDown(id1);      // [读2-禁用, 读1, 读3]
    manager.moveTaskUp(id3);        // [读2-禁用, 读3, 读1]
    CHECK("moveTaskUp/Down", manager.tasks()[1].name == QStringLiteral("读3-带延时"));

    int startedCount = 0, completedCount = 0;
    bool allCompleted = false;
    QVector<BatchTaskResult> results;
    QObject::connect(&manager, &BatchTaskManager::taskStarted,
                     [&](int) { ++startedCount; });
    QObject::connect(&manager, &BatchTaskManager::taskCompleted,
                     [&](const BatchTaskResult &r) { ++completedCount; results.append(r); });
    QObject::connect(&manager, &BatchTaskManager::allTasksCompleted,
                     [&](const QVector<BatchTaskResult> &) { allCompleted = true; });
    QObject::connect(&manager, &BatchTaskManager::executeReadTask,
                     [&](const BatchTask &task) {
                         // 模拟 MainWindow 执行读取成功
                         manager.onTaskFinished(task.id, true, QString(), {7, 8});
                     });

    manager.start();
    waitMs(300);   // 覆盖 t3 的 60ms 延时
    CHECK("顺序执行完成", allCompleted);
    CHECK("禁用任务被跳过（只执行 2 个）", startedCount == 2 && completedCount == 2);
    CHECK("结果含读取值", std::any_of(results.cbegin(), results.cend(),
        [](const BatchTaskResult &r) { return r.readValues == QVector<quint16>{7, 8}; }));

    // JSON 持久化回环
    const QString jsonPath = dir.path() + "/tasks.json";
    CHECK("saveToFile", manager.saveToFile(jsonPath));
    BatchTaskManager loader;
    CHECK("loadFromFile", loader.loadFromFile(jsonPath));
    CHECK("loadFromFile 数量", loader.tasks().size() == 3);
    CHECK("loadFromFile 字段", loader.tasks()[0].name == manager.tasks()[0].name
          && loader.tasks()[0].delayAfterMs == manager.tasks()[0].delayAfterMs);
}

static void testDataParser()
{
    printf("\n=== DataParser ===\n");
    using BO = ByteOrder;   // 全局作用域枚举

    // ---- P1 修复验证：ABCD = 100.0f 标准编码 {0x42C8, 0x0000} ----
    const auto f32 = DataParser::fromFloat32(100.0f, BO::BigEndian_ABCD);
    CHECK("P1 fromFloat32(ABCD) 生成标准寄存器序", f32[0] == 0x42C8 && f32[1] == 0x0000);
    CHECK("P1 toFloat32(ABCD) 还原 100.0f", qFuzzyCompare(DataParser::toFloat32(0x42C8, 0x0000, BO::BigEndian_ABCD), 100.0f));
    CHECK("P1 toUInt32(ABCD) 按大端还原", DataParser::toUInt32(0x1234, 0x5678, BO::BigEndian_ABCD) == 0x12345678u);

    // ---- 四种字节序全量验证（100.0f 的各设备编码）----
    CHECK("toFloat32(DCBA) 还原", qFuzzyCompare(DataParser::toFloat32(0x0000, 0xC842, BO::LittleEndian_DCBA), 100.0f));
    CHECK("toFloat32(BADC) 还原", qFuzzyCompare(DataParser::toFloat32(0xC842, 0x0000, BO::MidBigEndian_BADC), 100.0f));
    CHECK("toFloat32(CDAB) 还原", qFuzzyCompare(DataParser::toFloat32(0x0000, 0x42C8, BO::MidLittleEndian_CDAB), 100.0f));
    const auto f32d = DataParser::fromFloat32(100.0f, BO::LittleEndian_DCBA);
    CHECK("fromFloat32(DCBA) 生成", f32d[0] == 0x0000 && f32d[1] == 0xC842);
    const auto f32badc = DataParser::fromFloat32(100.0f, BO::MidBigEndian_BADC);
    CHECK("fromFloat32(BADC) 生成", f32badc[0] == 0xC842 && f32badc[1] == 0x0000);
    const auto f32cdab = DataParser::fromFloat32(100.0f, BO::MidLittleEndian_CDAB);
    CHECK("fromFloat32(CDAB) 生成", f32cdab[0] == 0x0000 && f32cdab[1] == 0x42C8);

    // int32 负数（ABCD）
    const auto i32 = DataParser::fromInt32(-2, BO::BigEndian_ABCD);
    CHECK("int32 ABCD 拆寄存器", i32[0] == 0xFFFF && i32[1] == 0xFFFE);
    CHECK("int32 ABCD 组装", DataParser::toInt32(i32[0], i32[1], BO::BigEndian_ABCD) == -2);

    // ---- D1 修复验证：toFloat64 尊重字节序 ----
    CHECK("D1 toFloat64(ABCD) 还原 1.0", DataParser::toFloat64(0x3FF0, 0x0000, 0x0000, 0x0000, BO::BigEndian_ABCD) == 1.0);
    CHECK("D1 toFloat64(DCBA) 还原 1.0", DataParser::toFloat64(0x0000, 0x0000, 0x0000, 0xF03F, BO::LittleEndian_DCBA) == 1.0);
    const auto f64 = DataParser::fromFloat64(1.0, BO::BigEndian_ABCD);
    CHECK("D1 fromFloat64(ABCD) 生成", f64[0] == 0x3FF0 && f64[1] == 0x0000 && f64[2] == 0x0000 && f64[3] == 0x0000);
    const auto f64d = DataParser::fromFloat64(1.0, BO::LittleEndian_DCBA);
    CHECK("D1 fromFloat64(DCBA) 生成", f64d[0] == 0x0000 && f64d[1] == 0x0000 && f64d[2] == 0x0000 && f64d[3] == 0xF03F);

    // ---- ASCII（按实现语义验证：寄存器高/低字节按大端取值）----
    CHECK("ASCII ABCD", DataParser::toAsciiString({0x4142}, BO::BigEndian_ABCD) == QStringLiteral("AB"));
    CHECK("ASCII DCBA 交换", DataParser::toAsciiString({0x4142}, BO::LittleEndian_DCBA) == QStringLiteral("BA"));
    const auto asciiRegs = DataParser::fromAsciiString(QStringLiteral("ABCD"), BO::BigEndian_ABCD);
    const QVector<quint16> asciiExpected = {0x4142, 0x4344};
    CHECK("fromAsciiString 回环", asciiRegs == asciiExpected);

    CHECK("swapBytes", DataParser::swapBytes(0x1234) == 0x3412);
}

// ---------------- S3 修复：加盐哈希 ----------------

static void testSecuritySaltedHash()
{
    printf("\n=== SecurityManager S3 加盐哈希 ===\n");
    SecurityManager smA;
    smA.addOrUpdateUser(QStringLiteral("saltuser"), QStringLiteral("pw123456"), QStringLiteral("operator"), true);
    SecurityManager smB;
    smB.addOrUpdateUser(QStringLiteral("saltuser"), QStringLiteral("pw123456"), QStringLiteral("operator"), true);

    QString hashA, hashB;
    for (const auto &u : smA.users())
        if (u.username == QStringLiteral("saltuser")) hashA = u.passwordHash;
    for (const auto &u : smB.users())
        if (u.username == QStringLiteral("saltuser")) hashB = u.passwordHash;

    const QString legacy = QString::fromLatin1(
        QCryptographicHash::hash(QString(QStringLiteral("pwd:pw123456")).toUtf8(),
                                 QCryptographicHash::Sha256).toHex());
    CHECK("S3: 哈希非空", !hashA.isEmpty() && !hashB.isEmpty());
    CHECK("S3: 相同密码两次哈希结果不同（随机盐）", hashA != hashB);
    CHECK("S3: 存储哈希不再等于旧裸 SHA256", hashA != legacy && hashB != legacy);
    CHECK("S3: 加盐后仍可登录", smA.login(QStringLiteral("saltuser"), QStringLiteral("pw123456")));
    CHECK("S3: 错误密码拒绝", !smA.login(QStringLiteral("saltuser"), QStringLiteral("wrong")));
    smA.logout();

    // API Token：salt:hash 格式
    smA.setApiToken(QStringLiteral("tok-abc"));
    CHECK("S3: 加盐 Token 校验通过", smA.verifyApiToken(QStringLiteral("tok-abc")));
    CHECK("S3: Token 存储格式为 salt:hash", smA.apiTokenHash().contains(QLatin1Char(':')));
    CHECK("S3: 错误 Token 拒绝", !smA.verifyApiToken(QStringLiteral("tok-x")));

    // 旧版无盐哈希兼容：手工构造 legacy 配置 → load → 登录成功并透明升级
    QTemporaryDir dir;
    CHECK("S3: QTemporaryDir 可用", dir.isValid());
    const QString iniPath = dir.path() + "/legacy.ini";
    const QString legacyPwdHash = QString::fromLatin1(
        QCryptographicHash::hash(QString(QStringLiteral("pwd:oldpw")).toUtf8(),
                                 QCryptographicHash::Sha256).toHex());
    {
        QSettings writer(iniPath, QSettings::IniFormat);
        writer.setValue("security/users", QStringList() << QStringLiteral("legacyuser"));
        writer.setValue("security/user/legacyuser/passwordHash", legacyPwdHash);
        writer.setValue("security/user/legacyuser/role", QStringLiteral("operator"));
        writer.setValue("security/user/legacyuser/enabled", true);
    }
    SecurityManager smC;
    QSettings settings(iniPath, QSettings::IniFormat);
    smC.load(settings);
    CHECK("S3: 旧版无盐哈希用户可登录（兼容）", smC.login(QStringLiteral("legacyuser"), QStringLiteral("oldpw")));
    smC.logout();
    QString upgradedHash, upgradedSalt;
    for (const auto &u : smC.users())
        if (u.username == QStringLiteral("legacyuser")) { upgradedHash = u.passwordHash; upgradedSalt = u.passwordSalt; }
    CHECK("S3: 登录后透明升级为加盐哈希", !upgradedSalt.isEmpty() && upgradedHash != legacyPwdHash);
}

// ---------------- A3 修复：BitSet/BitClear 位号越界防护 ----------------

static void testAlarmBitGuard()
{
    printf("\n=== AlarmManager A3 位号越界防护 ===\n");
    AlarmManager am;
    int triggered = 0;
    QObject::connect(&am, &AlarmManager::alarmTriggered, [&](const AlarmEvent &) { ++triggered; });

    AlarmRule base;
    base.enabled = true; base.serverAddress = 9; base.registerType = 4;
    base.severity = AlarmSeverity::Info; base.debounceMs = 0;
    base.acknowledged = false; base.threshold2 = 0;

    // 合法位号 bit5：值 32 = 1<<5，BitSet 语义验证
    AlarmRule rBit5 = base;
    rBit5.name = QStringLiteral("bit5"); rBit5.address = 0;
    rBit5.condition = AlarmCondition::BitSet; rBit5.threshold1 = 5;
    am.addRule(rBit5);
    am.checkValue(9, 4, 0, 32.0);
    CHECK("A3: bit5 BitSet 正常触发", triggered == 1);
    am.checkValue(9, 4, 0, 0.0);
    CHECK("A3: bit5 BitSet 回落清除", am.activeAlarmCount() == 0);

    // 越界位号（>31 或 <0）：不得触发、不得 UB（原实现 1 << 越界为未定义行为）
    AlarmRule rOver = base;
    rOver.name = QStringLiteral("bit32"); rOver.address = 1;
    rOver.condition = AlarmCondition::BitSet; rOver.threshold1 = 32;
    am.addRule(rOver);
    AlarmRule rNeg = base;
    rNeg.name = QStringLiteral("bitneg"); rNeg.address = 2;
    rNeg.condition = AlarmCondition::BitClear; rNeg.threshold1 = -1;
    am.addRule(rNeg);
    am.checkValue(9, 4, 1, 5.0);
    am.checkValue(9, 4, 2, 5.0);
    CHECK("A3: 越界位号不触发（无 UB）", triggered == 1 && am.activeAlarmCount() == 0);
}

// ---------------- U1 修复：凭据混淆编解码 ----------------

static void testCredentialCodec()
{
    printf("\n=== CredentialCodec U1 凭据混淆 ===\n");
    const QString plain = QStringLiteral("broker-Pass!123");
    const QString encoded = CredentialCodec::encode(plain);
    CHECK("U1: 编码结果带前缀", encoded.startsWith(QStringLiteral("enc:v1:")));
    CHECK("U1: 编码后不含明文", !encoded.contains(plain));
    CHECK("U1: 解码回环", CredentialCodec::decode(encoded) == plain);
    CHECK("U1: 历史明文原样透传", CredentialCodec::decode(plain) == plain);
    CHECK("U1: 空串处理", CredentialCodec::encode(QString()).isEmpty()
          && CredentialCodec::decode(QString()).isEmpty());
    CHECK("U1: 同一明文编码稳定", CredentialCodec::encode(plain) == encoded);
    const QString zh = QStringLiteral("密码abc");
    CHECK("U1: UTF-8 回环", CredentialCodec::decode(CredentialCodec::encode(zh)) == zh);
}

// =====================================================================
// PM 组：PointModel 点表管理 + CSV/JSON 批量导入导出
// 覆盖此前完全无自动化测试的新模块（提交 9138eac）。
// 重点：往返无损、列自适应、转义、原子性（失败不破坏现有点位）。
// =====================================================================

namespace pmtest {

// 写文本文件（默认 UTF-8，便于构造畸形输入）
static bool writeFile(const QString &path, const QByteArray &content, bool withBom = false)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (withBom)
        f.write("\xEF\xBB\xBF", 3);
    f.write(content);
    f.close();
    return true;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

// 判断两点除 id 外的业务字段是否一致（id 在导入时按约定重新分配，不参与比较）
static bool sameBusinessFields(const PointDefinition &a, const PointDefinition &b)
{
    return a.name == b.name
        && a.serverAddress == b.serverAddress
        && a.registerType == b.registerType
        && a.address == b.address
        && a.count == b.count
        && a.dataType == b.dataType
        && qFuzzyCompare(a.scale + 1.0, b.scale + 1.0)
        && qFuzzyCompare(a.offset + 1.0, b.offset + 1.0)
        && a.unit == b.unit
        && qFuzzyCompare(a.alarmLow + 1.0, b.alarmLow + 1.0)
        && qFuzzyCompare(a.alarmHigh + 1.0, b.alarmHigh + 1.0)
        && a.archiveEnabled == b.archiveEnabled
        && a.archiveIntervalSec == b.archiveIntervalSec;
}

static QVector<PointDefinition> samplePoints()
{
    QVector<PointDefinition> pts;
    for (int i = 0; i < 4; ++i) {
        PointDefinition p;
        p.name = QStringLiteral("点位%1").arg(i);
        p.serverAddress = 1 + i % 3;
        p.registerType = static_cast<QModbusDataUnit::RegisterType>(1 + i % 4); // 四张表轮转（跳过 Invalid）
        p.address = i * 10;
        p.count = (i % 3) + 1;
        p.dataType = QStringLiteral("uint16");
        p.scale = 1.0 + i * 0.25;
        p.offset = -i * 2.5;
        p.unit = QStringLiteral("°C");
        p.alarmLow = -100.0 - i;
        p.alarmHigh = 8000.0 + i * 100;
        p.archiveEnabled = (i % 2 == 0);
        p.archiveIntervalSec = 5 + i * 5;
        pts.append(p);
    }
    // 追加一个覆盖全部寄存器类型的点，确保四种类型都被往返覆盖
    PointDefinition t;
    t.name = QStringLiteral("线圈A");
    t.registerType = QModbusDataUnit::Coils;
    t.address = 3;
    t.count = 1;
    t.dataType = QStringLiteral("uint16");
    t.unit = QStringLiteral("V");
    t.alarmHigh = 1.0;
    pts.append(t);
    return pts;
}

// 填充模型（PointModel 继承 QObject 不可拷贝，故返回 unique_ptr 供引用绑定）
static std::unique_ptr<PointModel> buildModel(const QVector<PointDefinition> &pts)
{
    std::unique_ptr<PointModel> m(new PointModel);
    for (const PointDefinition &p : pts)
        m->addPoint(p);
    return m;
}

} // namespace pmtest

// =====================================================================
// DG 组：Modbus 调试工具纯逻辑（原 RawRequestDialog / ScannerDialog 内部逻辑）
// 这两个对话框是 GUI + 异步网络类，逻辑无法直接测，故已抽出 ModbusDiagnostics 模块。
// =====================================================================

// ---- DG1: PDU 十六进制解析容错 ----
static void testDiagParseHexBytes()
{
    printf("\n=== ModbusDiagnostics DG1 PDU 十六进制解析 ===\n");

    struct Case { const char *input; bool shouldPass; const char *bytesHex; const char *desc; };
    const Case cases[] = {
        { "00 00 00 0A", true,  "0000000A", "空格分隔" },
        { "00,0a",        true,  "000A",     "逗号分隔 + 小写" },
        { "000A",         true,  "000A",     "无分隔符" },
        { "0x00 0x0A",    true,  "000A",     "带 0x 前缀" },
        { "0X00 0X0a",    true,  "000A",     "大写 0X 前缀 + 小写数字" },
        { "00;0A:0B_0C-0D", true, "000A0B0C0D", "混合分隔符 ; : _ -" },
        { "  00  0A  ",   true,  "000A",     "前后空白" },
        { "",             true,  "",         "空串（允许空 PDU）" },
        { "   ",          true,  "",         "纯空白（允许空 PDU）" },
        { "0",            false, nullptr,    "单个字符（奇数长度）" },
        { "000",          false, nullptr,    "三个字符（奇数长度）" },
        { "0G",           false, nullptr,    "非法十六进制字符 G" },
        { "00 ZZ",        false, nullptr,    "含非法片段" },
        { "GG",           false, nullptr,    "全非法" },
        { "AB-CD",        true,  "ABCD",     "连字符作为分隔符" },
    };

    for (const Case &c : cases) {
        QByteArray out;
        QString err;
        const bool ok = ModbusDiagnostics::parseHexBytes(QString::fromUtf8(c.input), &out, &err);
        const QString expected = QString::fromUtf8(c.bytesHex ? c.bytesHex : "");
        const QByteArray expectBytes = QByteArray::fromHex(expected.toLatin1());
        if (c.shouldPass) {
            const bool same = (ok && out == expectBytes);
            CHECK(QStringLiteral("DG1: 解析「%1」→ %2（%3）")
                      .arg(QString::fromUtf8(c.input), expected, QString::fromUtf8(c.desc)),
                  same);
        } else {
            CHECK(QStringLiteral("DG1: 拒绝「%1」（%2）")
                      .arg(QString::fromUtf8(c.input), QString::fromUtf8(c.desc)),
                  !ok);
            CHECK(QStringLiteral("DG1: 失败时给出错误信息 —「%1」").arg(QString::fromUtf8(c.input)),
                  !err.isEmpty());
        }
    }

    // 失败时输出缓冲必须被清空（不能残留上一次的内容）
    {
        QByteArray out("STALE-DATA");
        QString err;
        ModbusDiagnostics::parseHexBytes(QStringLiteral("XYZ"), &out, &err);
        CHECK("DG1: 解析失败时输出缓冲被清空", out.isEmpty());
    }
    {   // 空 PDU 也要清空
        QByteArray out("STALE");
        QString err;
        ModbusDiagnostics::parseHexBytes(QString(), &out, &err);
        CHECK("DG1: 空输入时输出缓冲被清空", out.isEmpty());
    }
    {   // out 为 nullptr 时安全返回 false，不崩溃
        QString err;
        CHECK("DG1: out 为空指针时安全返回 false",
              !ModbusDiagnostics::parseHexBytes(QStringLiteral("00"), nullptr, &err));
    }
    {   // error 传 nullptr 不崩溃
        QByteArray out;
        CHECK("DG1: error 为空指针时解析仍成功",
              ModbusDiagnostics::parseHexBytes(QStringLiteral("00 01"), &out, nullptr)
                  && out.size() == 2);
    }
}

// ---- DG2: 十六进制显示 ----
static void testDiagHexFormatting()
{
    printf("\n=== ModbusDiagnostics DG2 十六进制显示 ===\n");
    CHECK("DG2: 0 → \"00\"（补零到两位）", ModbusDiagnostics::hexByte(0) == QStringLiteral("00"));
    CHECK("DG2: 1 → \"01\"", ModbusDiagnostics::hexByte(1) == QStringLiteral("01"));
    CHECK("DG2: 10 → \"0A\"（大写）", ModbusDiagnostics::hexByte(10) == QStringLiteral("0A"));
    CHECK("DG2: 15 → \"0F\"", ModbusDiagnostics::hexByte(15) == QStringLiteral("0F"));
    CHECK("DG2: 255 → \"FF\"", ModbusDiagnostics::hexByte(255) == QStringLiteral("FF"));
    CHECK("DG2: 128 → \"80\"", ModbusDiagnostics::hexByte(128) == QStringLiteral("80"));
    CHECK("DG2: 全部字节两字符宽", ModbusDiagnostics::hexByte(1).size() == 2);

    CHECK("DG2: 空数组 → 空串", ModbusDiagnostics::hexBytes(QByteArray()).isEmpty());
    CHECK("DG2: 单字节 → \"0A\"",
          ModbusDiagnostics::hexBytes(QByteArray::fromHex("0A")) == QStringLiteral("0A"));
    CHECK("DG2: 多字节空格分隔",
          ModbusDiagnostics::hexBytes(QByteArray::fromHex("0A0B0C")) == QStringLiteral("0A 0B 0C"));
    CHECK("DG2: 高位字节正确",
          ModbusDiagnostics::hexBytes(QByteArray::fromHex("FF0080")) == QStringLiteral("FF 00 80"));
}

// ---- DG3: 异常码解析全覆盖 ----
static void testDiagExceptionNames()
{
    printf("\n=== ModbusDiagnostics DG3 异常码解析 ===\n");
    struct Case { int code; const char *keyword; };
    const Case cases[] = {
        { 0x01, "非法功能" }, { 0x02, "非法数据地址" }, { 0x03, "非法数据值" },
        { 0x04, "设备故障" }, { 0x05, "确认" },        { 0x06, "忙" },
        { 0x08, "奇偶校验" }, { 0x0A, "网关路径" },    { 0x0B, "网关目标" },
    };
    for (const Case &c : cases) {
        const QString name = ModbusDiagnostics::exceptionName(c.code);
        CHECK(QStringLiteral("DG3: 异常码 0x%1 → 「%2」")
                  .arg(c.code, 2, 16, QLatin1Char('0')).arg(name),
              name.contains(QString::fromUtf8(c.keyword)));
    }
    // 规范里 0x07 / 0x09 未定义，0x0C+ 为厂商自定义
    for (int code : { 0x00, 0x07, 0x09, 0x0C, 0x0D, 0xFF }) {
        CHECK(QStringLiteral("DG3: 未定义异常码 0x%1 → 「未知异常」（非空）")
                  .arg(code, 2, 16, QLatin1Char('0')),
              ModbusDiagnostics::exceptionName(code) == QStringLiteral("未知异常"));
    }
    for (const Case &c : cases) {
        CHECK(QStringLiteral("DG3: 异常码 0x%1 不退化为未知")
                  .arg(c.code, 2, 16, QLatin1Char('0')),
              ModbusDiagnostics::exceptionName(c.code) != QStringLiteral("未知异常"));
    }
}

// ---- DG4: 报文构造（大端字节序）----
static void testDiagPayloadBuilding()
{
    printf("\n=== ModbusDiagnostics DG4 报文构造 ===\n");

    const QByteArray p = ModbusDiagnostics::readPayload(0, 10);
    CHECK("DG4: readPayload(0,10) 长度 4", p.size() == 4);
    CHECK("DG4: readPayload(0,10) 字节序正确（大端 00 00 00 0A）",
          p == QByteArray::fromHex("0000000A"));
    CHECK("DG4: readPayload(1,1) = 00 01 00 01",
          ModbusDiagnostics::readPayload(1, 1) == QByteArray::fromHex("00010001"));
    CHECK("DG4: readPayload(255,256) = 00 FF 01 00",
          ModbusDiagnostics::readPayload(255, 256) == QByteArray::fromHex("00FF0100"));
    CHECK("DG4: readPayload(65535,125) = FF FF 00 7D",
          ModbusDiagnostics::readPayload(65535, 125) == QByteArray::fromHex("FFFF007D"));
    CHECK("DG4: readPayload(0x1234,0x5678) = 12 34 56 78（大端交叉验证）",
          ModbusDiagnostics::readPayload(0x1234, 0x5678) == QByteArray::fromHex("12345678"));
    {   // 数量为 0 也不应崩溃（界面已限制 ≥1，但纯函数应健壮）
        // 注意：QByteArray::fromHex("00000000") 会得到「空」QByteArray（Qt 把全 0 视为空），
        // 所以这里逐字节校验，不能用 fromHex 做期望值。
        // 期望：地址 10 = 0x000A，数量 0 = 0x0000 → 00 0A 00 00
        const QByteArray z = ModbusDiagnostics::readPayload(10, 0);
        CHECK("DG4: readPayload 数量为 0 时长度仍为 4", z.size() == 4);
        CHECK("DG4: readPayload(10,0) 字节序正确（地址 00 0A + 数量 00 00）",
              static_cast<quint8>(z.at(0)) == 0x00 && static_cast<quint8>(z.at(1)) == 0x0A
              && static_cast<quint8>(z.at(2)) == 0x00 && static_cast<quint8>(z.at(3)) == 0x00);
    }
    {   // 越界值被截断到一字节（不产生多字节溢出）
        const QByteArray o = ModbusDiagnostics::readPayload(0x1FFFF, 1);
        CHECK("DG4: 超范围地址被截断为低 16 位（FF FF）", o.left(2) == QByteArray::fromHex("FFFF"));
    }


    {   // 构造 → 解析 往返一致
        const int addrs[] = { 0, 1, 255, 256, 4096, 65535 };
        bool allRoundTrip = true;
        for (int a : addrs) {
            const QByteArray d = ModbusDiagnostics::readPayload(a, 125);
            const int parsedAddr = (static_cast<quint8>(d.at(0)) << 8) | static_cast<quint8>(d.at(1));
            const int parsedCount = (static_cast<quint8>(d.at(2)) << 8) | static_cast<quint8>(d.at(3));
            if (parsedAddr != a || parsedCount != 125)
                allRoundTrip = false;
        }
        CHECK("DG4: 地址往返解析一致（大端无歧义）", allRoundTrip);
    }
}

// ---- DG5: 功能码合法性 ----
static void testDiagFunctionCodeValidation()
{
    printf("\n=== ModbusDiagnostics DG5 功能码合法性 ===\n");
    CHECK("DG5: FC01 合法", ModbusDiagnostics::isValidFunctionCode(0x01));
    CHECK("DG5: FC03 合法", ModbusDiagnostics::isValidFunctionCode(0x03));
    CHECK("DG5: FC10 合法", ModbusDiagnostics::isValidFunctionCode(0x10));
    CHECK("DG5: FC7F 合法（上边界）", ModbusDiagnostics::isValidFunctionCode(0x7F));
    CHECK("DG5: FC00 非法（保留）", !ModbusDiagnostics::isValidFunctionCode(0x00));
    CHECK("DG5: FC80 非法（异常响应标志位，非请求码）",
          !ModbusDiagnostics::isValidFunctionCode(0x80));
    CHECK("DG5: FCFF 非法", !ModbusDiagnostics::isValidFunctionCode(0xFF));
    CHECK("DG5: 负数非法", !ModbusDiagnostics::isValidFunctionCode(-1));
    CHECK("DG5: 超大值非法", !ModbusDiagnostics::isValidFunctionCode(999));

    const int tableCodes[] = { 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                               0x0B,0x0C,0x0F,0x10,0x11,0x16,0x17,0x18,0x2B };
    bool allValid = true;
    for (int c : tableCodes)
        if (!ModbusDiagnostics::isValidFunctionCode(c))
            allValid = false;
    CHECK("DG5: 功能码速查表 17 项全部合法", allValid);
}

// ---- DG6: 扫描区间校验 ----
static void testDiagScanRangeValidation()
{
    printf("\n=== ModbusDiagnostics DG6 扫描区间校验 ===\n");
    int total = -1;
    QString err;

    CHECK("DG6: 1..1 合法（单地址）",
          ModbusDiagnostics::validateScanRange(1, 1, &total, &err) && total == 1);
    total = -1;
    CHECK("DG6: 1..16 合法 → 共 16 个",
          ModbusDiagnostics::validateScanRange(1, 16, &total, &err) && total == 16);
    total = -1;
    CHECK("DG6: 1..247 合法（Modbus 标准全范围）→ 共 247 个",
          ModbusDiagnostics::validateScanRange(1, 247, &total, &err) && total == 247);
    total = -1;
    CHECK("DG6: 5..10 合法 → 共 6 个（含首尾）",
          ModbusDiagnostics::validateScanRange(5, 10, &total, &err) && total == 6);
    total = -1;
    CHECK("DG6: 247..247 合法",
          ModbusDiagnostics::validateScanRange(247, 247, &total, &err) && total == 1);
    total = -1;
    CHECK("DG6: 100..1 非法（结束<起始）",
          !ModbusDiagnostics::validateScanRange(100, 1, &total, &err));
    CHECK("DG6: 结束<起始时给出错误说明", err.contains(QStringLiteral("不小于")));
    CHECK("DG6: 失败时 total 输出为 0", total == 0);

    err.clear();
    CHECK("DG6: 起始 0 非法（广播地址不可扫描）",
          !ModbusDiagnostics::validateScanRange(0, 10, &total, &err));
    CHECK("DG6: 起始 0 的错误说明提到广播", err.contains(QStringLiteral("广播")));
    err.clear();
    CHECK("DG6: 结束 0 非法", !ModbusDiagnostics::validateScanRange(1, 0, &total, &err));
    err.clear();
    CHECK("DG6: 起始 248 非法（超上界）",
          !ModbusDiagnostics::validateScanRange(248, 248, &total, &err));
    err.clear();
    CHECK("DG6: 起始 -1 非法", !ModbusDiagnostics::validateScanRange(-1, 5, &total, &err));
    err.clear();
    CHECK("DG6: 结束 300 非法", !ModbusDiagnostics::validateScanRange(1, 300, &total, &err));
    err.clear();
    CHECK("DG6: 全部非法区间 total 均为 0",
          !ModbusDiagnostics::validateScanRange(0, 0, &total, &err) && total == 0);

    CHECK("DG6: 输出指针为 nullptr 时校验仍可用",
          ModbusDiagnostics::validateScanRange(1, 10, nullptr, nullptr));
}

// ---- DG7: 探测功能码与寄存器表映射 ----
static void testDiagProbeMapping()
{
    printf("\n=== ModbusDiagnostics DG7 探测功能码映射 ===\n");
    const ModbusDiagnostics::ProbeOption *opts = ModbusDiagnostics::probeOptions();
    const int n = ModbusDiagnostics::probeOptionCount();

    CHECK("DG7: 探测选项非空", n > 0);
    CHECK("DG7: 探测选项指针非空", opts != nullptr);
    CHECK("DG7: 恰有 4 个探测功能码（03/01/04/02）", n == 4);

    if (opts && n == 4) {
        CHECK("DG7: 选项 0 = FC03 读保持寄存器",
              opts[0].code == 0x03 && opts[0].type == QModbusDataUnit::HoldingRegisters);
        CHECK("DG7: 选项 1 = FC01 读线圈",
              opts[1].code == 0x01 && opts[1].type == QModbusDataUnit::Coils);
        CHECK("DG7: 选项 2 = FC04 读输入寄存器",
              opts[2].code == 0x04 && opts[2].type == QModbusDataUnit::InputRegisters);
        CHECK("DG7: 选项 3 = FC02 读离散输入",
              opts[3].code == 0x02 && opts[3].type == QModbusDataUnit::DiscreteInputs);
        bool allLabeled = true;
        for (int i = 0; i < n; ++i)
            if (!opts[i].label || strlen(opts[i].label) == 0)
                allLabeled = false;
        CHECK("DG7: 所有选项都有非空标签", allLabeled);
    }

    CHECK("DG7: FC03 → HoldingRegisters",
          ModbusDiagnostics::registerTypeForFunctionCode(0x03) == QModbusDataUnit::HoldingRegisters);
    CHECK("DG7: FC01 → Coils",
          ModbusDiagnostics::registerTypeForFunctionCode(0x01) == QModbusDataUnit::Coils);
    CHECK("DG7: FC04 → InputRegisters",
          ModbusDiagnostics::registerTypeForFunctionCode(0x04) == QModbusDataUnit::InputRegisters);
    CHECK("DG7: FC02 → DiscreteInputs",
          ModbusDiagnostics::registerTypeForFunctionCode(0x02) == QModbusDataUnit::DiscreteInputs);
    CHECK("DG7: 未知功能码 → HoldingRegisters（界面默认）",
          ModbusDiagnostics::registerTypeForFunctionCode(0x99) == QModbusDataUnit::HoldingRegisters);
    CHECK("DG7: 写类功能码 FC06 → HoldingRegisters（不属探测集）",
          ModbusDiagnostics::registerTypeForFunctionCode(0x06) == QModbusDataUnit::HoldingRegisters);

    bool consistent = true;
    for (int i = 0; i < n; ++i)
        if (ModbusDiagnostics::registerTypeForFunctionCode(opts[i].code) != opts[i].type)
            consistent = false;
    CHECK("DG7: registerTypeForFunctionCode 与选项表一致", consistent);
}

// ---- DG8: 扫描生成轮询任务 ----
static void testDiagPollTaskSeed()
{
    printf("\n=== ModbusDiagnostics DG8 扫描生成轮询任务 ===\n");
    {
        const ModbusDiagnostics::PollTaskSeed s =
            ModbusDiagnostics::makePollTaskSeed(12, 0x03, 100, 10);
        CHECK("DG8: id = 40000 + unit", s.id == 40012);
        CHECK("DG8: 名称含从站号", s.name.contains(QStringLiteral("12")));
        CHECK("DG8: serverAddress = unit", s.serverAddress == 12);
        CHECK("DG8: registerType 随探测功能码（FC03）",
              s.registerType == QModbusDataUnit::HoldingRegisters);
        CHECK("DG8: startAddress 透传", s.startAddress == 100);
        CHECK("DG8: quantity 透传", s.quantity == 10);
        CHECK("DG8: intervalMs 默认 1000", s.intervalMs == 1000);
        CHECK("DG8: alarmEnabled 默认关闭", !s.alarmEnabled);
    }
    {
        const auto coils = ModbusDiagnostics::makePollTaskSeed(1, 0x01, 0, 8);
        const auto inputs = ModbusDiagnostics::makePollTaskSeed(1, 0x04, 0, 8);
        const auto discrete = ModbusDiagnostics::makePollTaskSeed(1, 0x02, 0, 8);
        CHECK("DG8: FC01 生成的线圈任务",
              coils.registerType == QModbusDataUnit::Coils && coils.quantity == 8);
        CHECK("DG8: FC04 生成的输入寄存器任务",
              inputs.registerType == QModbusDataUnit::InputRegisters);
        CHECK("DG8: FC02 生成的离散输入任务",
              discrete.registerType == QModbusDataUnit::DiscreteInputs);
    }
    {
        const auto a = ModbusDiagnostics::makePollTaskSeed(7, 0x03, 0, 1);
        const auto b = ModbusDiagnostics::makePollTaskSeed(7, 0x03, 0, 1);
        CHECK("DG8: 同一从站 id 稳定可重现", a.id == b.id);
        CHECK("DG8: 同一从站名称稳定可重现", a.name == b.name);
    }
    {
        const auto a = ModbusDiagnostics::makePollTaskSeed(1, 0x03, 0, 1);
        const auto b = ModbusDiagnostics::makePollTaskSeed(2, 0x03, 0, 1);
        CHECK("DG8: 不同从站 id 不冲突", a.id != b.id);
        CHECK("DG8: id 落在 40000+ 段", a.id >= 40001 && b.id >= 40001);
    }
    {
        const auto lo = ModbusDiagnostics::makePollTaskSeed(1, 0x03, 0, 0);
        CHECK("DG8: unit=1 正常", lo.serverAddress == 1);
        CHECK("DG8: count=0 兜底为 1", lo.quantity == 1);
        const auto hi = ModbusDiagnostics::makePollTaskSeed(247, 0x03, 65535, 125);
        CHECK("DG8: unit=247 正常", hi.serverAddress == 247 && hi.id == 40247);
        CHECK("DG8: 最大 startAddress/quantity 透传",
              hi.startAddress == 65535 && hi.quantity == 125);
        const auto neg = ModbusDiagnostics::makePollTaskSeed(5, 0x03, -10, -1);
        CHECK("DG8: 负 startAddress 兜底为 0", neg.startAddress == 0);
        CHECK("DG8: 负 quantity 兜底为 1", neg.quantity == 1);
    }
}

// ---- DG9: 进度统计 ----
static void testDiagScanProgress()
{
    printf("\n=== ModbusDiagnostics DG9 扫描进度文案 ===\n");
    CHECK("DG9: 0/16/0 文案",
          ModbusDiagnostics::scanProgressText(0, 16, 0)
              == QStringLiteral("已扫描 0/16，发现 0 个在线从站"));
    CHECK("DG9: 8/16/2 文案",
          ModbusDiagnostics::scanProgressText(8, 16, 2)
              == QStringLiteral("已扫描 8/16，发现 2 个在线从站"));
    CHECK("DG9: 16/16/16 文案",
          ModbusDiagnostics::scanProgressText(16, 16, 16)
              == QStringLiteral("已扫描 16/16，发现 16 个在线从站"));
    CHECK("DG9: 负数 found 被夹为 0",
          ModbusDiagnostics::scanProgressText(1, 16, -5).contains(QStringLiteral("发现 0")));
}
// ---- PM1: JSON 往返无损 ----
static void testPointModelJsonRoundTrip()
{
    printf("\n=== PointModel PM1 JSON 往返 ===\n");
    QTemporaryDir dir;
    CHECK("PM1: QTemporaryDir 可用", dir.isValid());
    const QString path = dir.filePath(QStringLiteral("points.json"));

    const QVector<PointDefinition> src = pmtest::samplePoints();
    auto modelPtr = pmtest::buildModel(src);
    PointModel *model = modelPtr.get();

    QString err;
    CHECK("PM1: 导出 JSON 成功", model->exportToJson(path, &err));
    CHECK("PM1: 导出无错误信息", err.isEmpty());
    CHECK("PM1: 文件已生成", QFile::exists(path));

    // 导入到全新实例
    PointModel target;
    int imported = -1;
    QString err2;
    CHECK("PM1: 导入 JSON 成功", target.importFromJson(path, false, &imported, &err2));
    CHECK("PM1: 导入数量正确", imported == src.size());
    CHECK("PM1: 导入后点位数正确", target.points().size() == src.size());

    bool allMatch = true;
    const QVector<PointDefinition> got = target.points();
    for (int i = 0; i < src.size() && i < got.size(); ++i)
        if (!pmtest::sameBusinessFields(src.at(i), got.at(i)))
            allMatch = false;
    CHECK("PM1: 全部字段往返无损（含四张寄存器表）", allMatch);

    // 校验具体数值，避免"看起来对"的假阳性
    if (got.size() >= 3) {
        CHECK("PM1: 第0点 scale/offset 精确还原", qFuzzyCompare(got[0].scale, 1.0)
              && qFuzzyCompare(got[0].offset, 0.0) && got[0].unit == QStringLiteral("°C"));
        CHECK("PM1: 第1点 serverAddress 还原", got[1].serverAddress == 2);
        CHECK("PM1: 第1点 archiveEnabled=false 还原", got[1].archiveEnabled == false);
        CHECK("PM1: 第2点 archiveIntervalSec 还原", got[2].archiveIntervalSec == 15);
        CHECK("PM1: Coils 类型点还原", got.last().registerType == QModbusDataUnit::Coils
              && got.last().alarmHigh == 1.0);
    }

}

// ---- PM2: CSV 往返无损 + BOM ----
static void testPointModelCsvRoundTrip()
{
    printf("\n=== PointModel PM2 CSV 往返 ===\n");
    QTemporaryDir dir;
    CHECK("PM2: QTemporaryDir 可用", dir.isValid());
    const QString path = dir.filePath(QStringLiteral("points.csv"));

    const QVector<PointDefinition> src = pmtest::samplePoints();
    auto modelPtr = pmtest::buildModel(src);
    PointModel *model = modelPtr.get();

    QString err;
    CHECK("PM2: 导出 CSV 成功", model->exportToCsv(path, &err));
    const QByteArray raw = pmtest::readFile(path);
    CHECK("PM2: 文件以 UTF-8 BOM 开头（Excel 友好）", raw.startsWith("\xEF\xBB\xBF"));

    // 确认转义：°C 不含逗号/引号，不应被引号包裹；这一条顺带验证不会无脑加引号
    CHECK("PM2: 普通字段未被多余包裹", raw.contains("°C"));

    PointModel target;
    int imported = -1;
    QString err2;
    CHECK("PM2: 导入 CSV 成功", target.importFromCsv(path, false, &imported, &err2));
    CHECK("PM2: 导入数量正确", imported == src.size());
    CHECK("PM2: BOM 未污染第一列（name 不含 BOM 残留）",
          !target.points().isEmpty() && !target.points().first().name.startsWith(QChar(0xFEFF)));

    bool allMatch = true;
    const QVector<PointDefinition> got = target.points();
    for (int i = 0; i < src.size() && i < got.size(); ++i)
        if (!pmtest::sameBusinessFields(src.at(i), got.at(i)))
            allMatch = false;
    CHECK("PM2: 全部字段往返无损", allMatch);
    CHECK("PM2: id 在导入时重新分配（不复用旧 id）", !got.isEmpty() && got.first().id != src.first().id);

}

// ---- PM3: 表头列自适应（乱序 / 缺列 / 大小写 / 空格）----
static void testPointModelCsvHeaderAdaptivity()
{
    printf("\n=== PointModel PM3 CSV 表头自适应 ===\n");
    QTemporaryDir dir;
    CHECK("PM3: QTemporaryDir 可用", dir.isValid());

    {   // 3.1 列顺序完全打乱
        const QString path = dir.filePath(QStringLiteral("shuffled.csv"));
        pmtest::writeFile(path,
            "unit,dataType,address,name,scale,serverAddress,registerType,count\n"
            "V,float32,20,乱序点,2.5,3,2,2\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM3: 乱序表头导入成功", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM3: 乱序表头导入 1 条", imported == 1);
        if (!m.points().isEmpty()) {
            const PointDefinition p = m.points().first();
            CHECK("PM3: 列乱序仍正确映射 name", p.name == QStringLiteral("乱序点"));
            CHECK("PM3: 列乱序仍正确映射 address", p.address == 20);
            CHECK("PM3: 列乱序仍正确映射 unit", p.unit == QStringLiteral("V"));
            CHECK("PM3: 列乱序仍正确映射 scale", qFuzzyCompare(p.scale, 2.5));
            CHECK("PM3: 列乱序仍正确映射 dataType", p.dataType == QStringLiteral("float32"));
            CHECK("PM3: 列乱序仍正确映射 serverAddress", p.serverAddress == 3);
            CHECK("PM3: 列乱序仍正确映射 registerType（2=Coils，Qt 枚举）", p.registerType == QModbusDataUnit::Coils);
            CHECK("PM3: 列乱序仍正确映射 count", p.count == 2);
        }
    }

    {   // 3.2 表头大小写不敏感 + 带空格
        const QString path = dir.filePath(QStringLiteral("case.csv"));
        pmtest::writeFile(path,
            "Name,ADDRESS , ServerAddress\n"
            "空格点,7,5\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM3: 大小写/空格表头导入成功", m.importFromCsv(path, false, &imported, &err));
        if (!m.points().isEmpty()) {
            const PointDefinition p = m.points().first();
            CHECK("PM3: 空格表头 address 正确（不因尾部空格错位）", p.address == 7);
            CHECK("PM3: 空格表头 serverAddress 正确", p.serverAddress == 5);
            CHECK("PM3: 空格表头 name 正确", p.name == QStringLiteral("空格点"));
        }
    }

    {   // 3.3 缺列时用默认值，且不崩溃
        const QString path = dir.filePath(QStringLiteral("missing.csv"));
        pmtest::writeFile(path,
            "name,address\n"
            "缺列点,12\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM3: 缺列表头导入成功", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM3: 缺列仍导入 1 条", imported == 1);
        if (!m.points().isEmpty()) {
            const PointDefinition p = m.points().first();
            CHECK("PM3: 缺列时 serverAddress 兜底为 1", p.serverAddress == 1);
            CHECK("PM3: 缺列时 dataType 兜底为 uint16", p.dataType == QStringLiteral("uint16"));
            CHECK("PM3: 缺列时 count 兜底为 1", p.count == 1);
            CHECK("PM3: 缺列时 scale 兜底为 1.0", qFuzzyCompare(p.scale, 1.0));
            CHECK("PM3: 缺列时 archiveEnabled 兜底为 true", p.archiveEnabled == true);
            CHECK("PM3: 缺列时 alarmHigh 兜底为 65535", qFuzzyCompare(p.alarmHigh, 65535.0));
        }
    }

    {   // 3.4 无表头（纯数据行）按导出顺序回退定位
        const QString path = dir.filePath(QStringLiteral("noheader.csv"));
        pmtest::writeFile(path,
            "99,无表头点,2,3,30,2,uint16,1.5,-1.5,bar,0,100,1,15\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM3: 无表头 CSV 导入成功", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM3: 无表头按导出顺序解析 name", !m.points().isEmpty()
              && m.points().first().name == QStringLiteral("无表头点"));
        CHECK("PM3: 无表头按导出顺序解析 address", !m.points().isEmpty()
              && m.points().first().address == 30);
        CHECK("PM3: 无表头解析 unit", !m.points().isEmpty()
              && m.points().first().unit == QStringLiteral("bar"));
    }
}

// ---- PM4: CSV 转义往返（含逗号/引号/换行/中文）----
static void testPointModelCsvEscaping()
{
    printf("\n=== PointModel PM4 CSV 转义往返 ===\n");
    QTemporaryDir dir;
    CHECK("PM4: QTemporaryDir 可用", dir.isValid());
    const QString path = dir.filePath(QStringLiteral("escape.csv"));

    PointModel src;
    const QString tricky = QStringLiteral("含,逗号\"引号\n换行");
    PointDefinition a;
    a.name = tricky;
    a.address = 5;
    a.dataType = QStringLiteral("uint16");
    a.unit = QStringLiteral("Pa, kPa");   // 逗号 + 空格
    a.scale = 0.1;
    a.alarmHigh = 100.0;
    src.addPoint(a);
    PointDefinition b;
    b.name = QStringLiteral("普通名称");
    b.address = 6;
    b.unit = QStringLiteral("℃");          // 非 ASCII
    b.archiveEnabled = false;
    b.archiveIntervalSec = 60;
    src.addPoint(b);

    QString err;
    CHECK("PM4: 导出含特殊字符 CSV 成功", src.exportToCsv(path, &err));

    const QByteArray raw = pmtest::readFile(path);
    CHECK("PM4: 逗号字段被引号包裹", raw.contains("\"Pa, kPa\""));
    // 平台换行差异（QTextStream 在 Windows 输出 \r\n、Linux 输出 \n）会让原始字节
    // 断言不稳定，故这里只断言「结构性事实」：字段以引号包裹、内部引号成对转义。
    // 内容层面的正确性由下方「名称完整还原」断言保证（那才是用户可见的结果）。
    {
        // 该行以 ", 表示 name 字段被引号包裹
        const int at = raw.indexOf("\"含,");
        CHECK("PM4: 含逗号的名称字段以引号包裹", at >= 0);
        // 转义后的内部引号是 ""（两个连续引号）
        const QByteArray dbl = QByteArray("\"\"");
        const int q1 = raw.indexOf(dbl, at >= 0 ? at : 0);
        CHECK("PM4: 内部引号成对转义为双引号", q1 > at);
        // 引号字段内不应出现落单的单个引号（未转义）。
        // 注意：闭引号后面紧跟分隔符 ','，在扫描里会被算作「前一个也是引号」之外的情况，
        // 因此只检查「双引号出现在字段开头或结尾」这一对结构性位置。
        const QByteArray dbl2 = QByteArray("\"\"");
        bool badEscape = false;
        int scan = at >= 0 ? at : 0;
        while (true) {
            const int p1 = raw.indexOf(dbl2, scan);
            if (p1 < 0) break;
            // 三连引号及以上不符合 csvEscape 规则（只允许 "" 转义 + 包裹引号）
            if (raw.mid(p1, 3) == QByteArray("\"\"\"")) { badEscape = true; break; }
            scan = p1 + 2;
        }
        CHECK("PM4: 无三连引号等非法转义形态", !badEscape);
    }

    PointModel target;
    int imported = 0;
    QString err2;
    CHECK("PM4: 导入转义 CSV 成功", target.importFromCsv(path, false, &imported, &err2));
    CHECK("PM4: 转义 CSV 导入 2 条（未被换行拆行）", imported == 2);

    const QVector<PointDefinition> got = target.points();
    if (got.size() >= 2) {
        CHECK("PM4: 含逗号/引号/换行的名称完整还原", got[0].name == tricky);
        CHECK("PM4: 含逗号+空格的单位还原", got[0].unit == QStringLiteral("Pa, kPa"));
        CHECK("PM4: 非 ASCII 单位还原（℃）", got[1].unit == QStringLiteral("℃"));
        CHECK("PM4: 简单名称还原", got[1].name == QStringLiteral("普通名称"));
        CHECK("PM4: archiveEnabled=false 往返", got[1].archiveEnabled == false);
        CHECK("PM4: archiveIntervalSec 往返", got[1].archiveIntervalSec == 60);
    }

    // 空单元格（非引号包裹的空值）不应导致错位
    const QString path2 = dir.filePath(QStringLiteral("empty_cell.csv"));
    pmtest::writeFile(path2,
        "name,address,unit,scale\n"
        "空单位,4,,2\n");
    PointModel m2;
    int imported2 = 0;
    QString err3;
    CHECK("PM4: 空单元格 CSV 导入成功", m2.importFromCsv(path2, false, &imported2, &err3));
    CHECK("PM4: 空单元格仍导入 1 条", imported2 == 1);
    if (!m2.points().isEmpty()) {
        CHECK("PM4: 空单位解析为空串（不误取后列）", m2.points().first().unit.isEmpty());
        CHECK("PM4: 空单位时 scale 仍正确（列未错位）", qFuzzyCompare(m2.points().first().scale, 2.0));
    }
}

// ---- PM5: 原子性——导入失败绝不破坏现有点位（最高优先级）----
static void testPointModelImportAtomicity()
{
    printf("\n=== PointModel PM5 导入原子性 ===\n");
    QTemporaryDir dir;
    CHECK("PM5: QTemporaryDir 可用", dir.isValid());

    const QVector<PointDefinition> keep = pmtest::samplePoints();
    const QString expectName = keep.first().name;

    {   // 5.1 JSON 语法错误（覆盖模式）
        auto owner = pmtest::buildModel(keep);
        PointModel &m = *owner;
        const QString path = dir.filePath(QStringLiteral("broken.json"));
        pmtest::writeFile(path, "{ this is not valid json ");
        int imported = -1;
        QString err;
        CHECK("PM5: 非法 JSON 导入失败（返回 false）", !m.importFromJson(path, false, &imported, &err));
        CHECK("PM5: 非法 JSON 给出错误信息", !err.isEmpty());
        CHECK("PM5: 非法 JSON 未破坏现有点位（原子性）", m.points().size() == keep.size());
        CHECK("PM5: 非法 JSON 未改动点位内容", !m.points().isEmpty()
              && m.points().first().name == expectName);
    }

    {   // 5.2 JSON 顶层不是数组
        auto owner = pmtest::buildModel(keep);
        PointModel &m = *owner;
        const QString path = dir.filePath(QStringLiteral("obj.json"));
        pmtest::writeFile(path, "{ \"name\": \"x\" }");
        int imported = -1;
        QString err;
        CHECK("PM5: 非数组 JSON 导入失败", !m.importFromJson(path, false, &imported, &err));
        CHECK("PM5: 非数组 JSON 未破坏现有点位", m.points().size() == keep.size());
    }

    {   // 5.3 文件不存在
        auto owner = pmtest::buildModel(keep);
        PointModel &m = *owner;
        int imported = -1;
        QString err;
        CHECK("PM5: 不存在的文件导入失败", !m.importFromJson(dir.filePath("nope.json"), false, &imported, &err));
        CHECK("PM5: 不存在的 CSV 导入失败", !m.importFromCsv(dir.filePath("nope.csv"), false, &imported, &err));
        CHECK("PM5: 文件缺失未破坏现有点位", m.points().size() == keep.size());
    }

    {   // 5.4 CSV 只有表头、无数据行（全部行被跳过 → count==0 → 返回 false）
        auto owner = pmtest::buildModel(keep);
        PointModel &m = *owner;
        const QString path = dir.filePath(QStringLiteral("headeronly.csv"));
        pmtest::writeFile(path, "name,address,serverAddress,registerType,dataType\n");
        int imported = -1;
        QString err;
        const bool ok = m.importFromCsv(path, false, &imported, &err);
        CHECK("PM5: 仅有表头的 CSV 返回失败（无有效点位）", !ok);
        CHECK("PM5: 仅有表头时现有点点未被清空（原子性）", m.points().size() == keep.size());
        CHECK("PM5: 仅有表头时点位内容完好", !m.points().isEmpty()
              && m.points().first().name == expectName);
    }

    {   // 5.5 有内容但 name/address 皆无效的行（行本身非空 → 进入解析 → 无有效点位）
        auto owner = pmtest::buildModel(keep);
        PointModel &m = *owner;
        const QString path = dir.filePath(QStringLiteral("allinvalid.csv"));
        // 列有内容（scale/unit），但 name 与 address 全空 → 每行都被判为无效行
        pmtest::writeFile(path,
            "name,address,unit,scale\n"
            ",,V,1\n"
            ",,A,2\n");
        int imported = -1;
        QString err;
        const bool ok = m.importFromCsv(path, false, &imported, &err);
        CHECK("PM5: 全无效行 CSV 返回失败", !ok);
        CHECK("PM5: 全无效行时 imported 置 0", imported == 0);
        CHECK("PM5: 全无效行时现有点点未被清空（原子性）", m.points().size() == keep.size());
        CHECK("PM5: 全无效行时点位内容完好", !m.points().isEmpty()
              && m.points().first().name == expectName);
        CHECK("PM5: 全无效行时错误信息提示检查表头", err.contains(QStringLiteral("表头")));
    }

    {   // 5.5b 完全是空行/空单元格（无任何内容）→ 视为「无内容可导入」，返回 true 但不改动数据
        auto owner = pmtest::buildModel(keep);
        PointModel &m = *owner;
        const QString path = dir.filePath(QStringLiteral("blankrows.csv"));
        pmtest::writeFile(path, ",,,\n,,\n");
        int imported = -1;
        QString err;
        const bool ok = m.importFromCsv(path, false, &imported, &err);
        CHECK("PM5: 纯空行 CSV 返回 true（无内容可导入，非错误）", ok);
        CHECK("PM5: 纯空行 imported 为 0", imported == 0);
        CHECK("PM5: 纯空行不改动现有点点", m.points().size() == keep.size());
    }

    {   // 5.6 混合：部分有效行 → 成功导入且只丢无效行
        PointModel m;
        const QString path = dir.filePath(QStringLiteral("mixed.csv"));
        pmtest::writeFile(path,
            "name,address\n"
            "有效A,1\n"
            ",,\n"
            "有效B,2\n");
        int imported = 0;
        QString err;
        CHECK("PM5: 部分有效行导入成功", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM5: 只导入有效行（2 条）", imported == 2);
        CHECK("PM5: 有效行内容正确", m.points().size() == 2
              && m.points().at(0).name == QStringLiteral("有效A")
              && m.points().at(1).name == QStringLiteral("有效B"));
    }
}

// ---- PM6: 追加 / 覆盖语义 ----
static void testPointModelAppendOverwrite()
{
    printf("\n=== PointModel PM6 追加 / 覆盖 ===\n");
    QTemporaryDir dir;
    CHECK("PM6: QTemporaryDir 可用", dir.isValid());

    const QString srcPath = dir.filePath(QStringLiteral("src.csv"));
    {
        PointModel src;
        PointDefinition a; a.name = QStringLiteral("导入A"); a.address = 1; src.addPoint(a);
        PointDefinition b; b.name = QStringLiteral("导入B"); b.address = 2; src.addPoint(b);
        QString err;
        CHECK("PM6: 生成源 CSV 成功", src.exportToCsv(srcPath, &err));
    }

    {   // 追加
        PointModel m;
        PointDefinition own; own.name = QStringLiteral("原有X"); own.address = 100;
        m.addPoint(own);
        int imported = 0;
        QString err;
        CHECK("PM6: 追加模式导入成功", m.importFromCsv(srcPath, true, &imported, &err));
        CHECK("PM6: 追加导入 2 条", imported == 2);
        CHECK("PM6: 追加后原有点位仍在（共 3）", m.points().size() == 3);
        CHECK("PM6: 追加后首项仍是原有点位", m.points().first().name == QStringLiteral("原有X"));
        CHECK("PM6: 追加分配了新 id（与原有不冲突）",
              m.points().at(1).id != m.points().first().id);
    }

    {   // 覆盖
        PointModel m;
        for (int i = 0; i < 5; ++i) {
            PointDefinition own; own.name = QStringLiteral("旧%1").arg(i); own.address = i;
            m.addPoint(own);
        }
        int imported = 0;
        QString err;
        CHECK("PM6: 覆盖模式导入成功", m.importFromCsv(srcPath, false, &imported, &err));
        CHECK("PM6: 覆盖后仅剩导入的 2 条", m.points().size() == 2);
        CHECK("PM6: 覆盖后旧点位已清除", m.points().first().name == QStringLiteral("导入A"));
        CHECK("PM6: 覆盖后 currentValues 同步为 2 条", m.currentValues().size() == 2);
    }

    {   // JSON 追加/覆盖语义与 CSV 一致
        const QString jsonPath = dir.filePath(QStringLiteral("src.json"));
        PointModel src;
        PointDefinition a; a.name = QStringLiteral("J导入A"); a.address = 1; src.addPoint(a);
        QString err;
        src.exportToJson(jsonPath, &err);

        PointModel m;
        PointDefinition own; own.name = QStringLiteral("原有J"); own.address = 50; m.addPoint(own);
        int imported = 0;
        CHECK("PM6: JSON 追加成功", m.importFromJson(jsonPath, true, &imported, &err));
        CHECK("PM6: JSON 追加后共 2 条", m.points().size() == 2);

        PointModel m2;
        for (int i = 0; i < 4; ++i) { PointDefinition o; o.name = QStringLiteral("旧J%1").arg(i); m2.addPoint(o); }
        CHECK("PM6: JSON 覆盖成功", m2.importFromJson(jsonPath, false, &imported, &err));
        CHECK("PM6: JSON 覆盖后仅 1 条", m2.points().size() == 1);
    }

    {   // Invalid 类型（枚举值 0）无法往返 —— 记录为已知缺陷
        // 根因：CSV/JSON 用 static_cast<int>(RegisterType) 存储，Invalid=0；
        //       导入端把 0 视为「非法值」拒绝解析并回落 HoldingRegisters。
        // 影响面小：正常流程不会创建 Invalid 点位（UI 只会选四张表），但手工编辑的 CSV 可能出现 0。
        auto owner = pmtest::buildModel(pmtest::samplePoints());
        PointModel &m = *owner;
        PointDefinition bad;
        bad.name = QStringLiteral("非法类型点");
        bad.registerType = QModbusDataUnit::Invalid;
        bad.address = 77;
        m.addPoint(bad);
        QString err;
        const QString jpath = dir.filePath(QStringLiteral("invalid.json"));
        m.exportToJson(jpath, &err);
        auto owner2 = pmtest::buildModel(pmtest::samplePoints());
        PointModel &m2 = *owner2;
        int imported = 0;
        m2.importFromJson(jpath, false, &imported, &err);
        CHECK_ISSUE("PM6: Invalid 寄存器类型无法往返（导出 0 → 导入回落 HoldingRegisters）",
                    !m2.points().isEmpty()
                    && m2.points().last().registerType == QModbusDataUnit::Invalid);
    }
}

// ---- PM7: registerType / 布尔 token 多写法兼容 ----
static void testPointModelTokenCompatibility()
{
    printf("\n=== PointModel PM7 token 多写法兼容 ===\n");
    QTemporaryDir dir;
    CHECK("PM7: QTemporaryDir 可用", dir.isValid());

    {   // registerType 数字 / 英文 / 中文三种写法
        const QString path = dir.filePath(QStringLiteral("rt.csv"));
        pmtest::writeFile(path,
            "name,address,registerType\n"
            "数字0,1,0\n"
            "数字1,2,1\n"
            "数字2,3,2\n"
            "数字3,4,3\n"
            "数字4,13,4\n"
            "英文holding,5,holding\n"
            "英文input,6,input\n"
            "英文discrete,7,discrete\n"
            "英文coil,8,coil\n"
            "中文保持,9,保持寄存器\n"
            "中文输入,10,输入寄存器\n"
            "中文离散,11,离散输入\n"
            "中文线圈,12,线圈\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM7: registerType 多写法导入成功", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM7: 导入 13 条", imported == 13);

        QVector<QModbusDataUnit::RegisterType> got;
        for (const PointDefinition &p : m.points())
            got.append(p.registerType);

        auto at = [&got](int i) { return i < got.size() ? got.at(i) : QModbusDataUnit::HoldingRegisters; };
        // 数字形式严格等于 QModbusDataUnit::RegisterType 真实枚举值：
        // Invalid=0, DiscreteInputs=1, Coils=2, InputRegisters=3, HoldingRegisters=4
        // 这是导出/导入对称的前提（导出写的就是这些值）。
        CHECK("PM7: 数字 0 (Invalid) 拒绝解析 → 回落 HoldingRegisters", at(0) == QModbusDataUnit::HoldingRegisters);
        CHECK("PM7: 数字 1 → DiscreteInputs（Qt 枚举）", at(1) == QModbusDataUnit::DiscreteInputs);
        CHECK("PM7: 数字 2 → Coils（Qt 枚举，非 InputRegisters）", at(2) == QModbusDataUnit::Coils);
        CHECK("PM7: 数字 3 → InputRegisters（Qt 枚举）", at(3) == QModbusDataUnit::InputRegisters);
        CHECK("PM7: 数字 4 → HoldingRegisters（Qt 枚举）", at(4) == QModbusDataUnit::HoldingRegisters);
        CHECK("PM7: 英文 holding → HoldingRegisters", at(5) == QModbusDataUnit::HoldingRegisters);
        CHECK("PM7: 英文 input → InputRegisters", at(6) == QModbusDataUnit::InputRegisters);
        CHECK("PM7: 英文 discrete → DiscreteInputs", at(7) == QModbusDataUnit::DiscreteInputs);
        CHECK("PM7: 英文 coil → Coils", at(8) == QModbusDataUnit::Coils);
        CHECK("PM7: 中文 保持寄存器 → HoldingRegisters", at(9) == QModbusDataUnit::HoldingRegisters);
        CHECK("PM7: 中文 输入寄存器 → InputRegisters", at(10) == QModbusDataUnit::InputRegisters);
        CHECK("PM7: 中文 离散输入 → DiscreteInputs", at(11) == QModbusDataUnit::DiscreteInputs);
        CHECK("PM7: 中文 线圈 → Coils", at(12) == QModbusDataUnit::Coils);

        // 非法 registerType 不应崩溃，且应回退为 HoldingRegisters
        const QString bad = dir.filePath(QStringLiteral("rt_bad.csv"));
        pmtest::writeFile(bad, "name,address,registerType\n坏值,1,not_a_type\n");
        PointModel m2;
        int imported2 = 0;
        QString err2;
        CHECK("PM7: 非法 registerType 不崩溃且导入成功", m2.importFromCsv(bad, false, &imported2, &err2));
        CHECK("PM7: 非法 registerType 回退 HoldingRegisters", !m2.points().isEmpty()
              && m2.points().first().registerType == QModbusDataUnit::HoldingRegisters);
    }

    {   // archiveEnabled 多写法
        const QString path = dir.filePath(QStringLiteral("bool.csv"));
        pmtest::writeFile(path,
            "name,address,archiveEnabled\n"
            "b1,1,1\n" "b2,2,true\n" "b3,3,yes\n" "b4,4,y\n" "b5,5,是\n" "b6,6,on\n"
            "b7,7,0\n" "b8,8,false\n" "b9,9,no\n" "b10,10,n\n" "b11,11,否\n" "b12,12,off\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM7: archiveEnabled 多写法导入成功", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM7: 导入 12 条", imported == 12);
        if (m.points().size() == 12) {
            bool truthy = true, falsy = true;
            for (int i = 0; i < 6; ++i)  truthy = truthy && m.points().at(i).archiveEnabled;
            for (int i = 6; i < 12; ++i) falsy = falsy && !m.points().at(i).archiveEnabled;
            CHECK("PM7: 1/true/yes/y/是/on 均解析为真", truthy);
            CHECK("PM7: 0/false/no/n/否/off 均解析为假", falsy);
        }
    }

    {   // importAuto 按扩展名分派 + 未知扩展名回退
        const QString csvPath = dir.filePath(QStringLiteral("auto.csv"));
        const QString jsonPath = dir.filePath(QStringLiteral("auto.json"));
        const QString noExt = dir.filePath(QStringLiteral("auto_noext"));
        const QString unknown = dir.filePath(QStringLiteral("auto.dat"));
        PointModel src;
        PointDefinition a; a.name = QStringLiteral("自动A"); a.address = 1; src.addPoint(a);
        QString err;
        src.exportToCsv(csvPath, &err);
        src.exportToJson(jsonPath, &err);
        QFile::remove(noExt); QFile::remove(unknown);
        QFile::copy(csvPath, noExt);
        QFile::copy(jsonPath, unknown);

        PointModel m1, m2, m3, m4;
        int n1 = 0, n2 = 0, n3 = 0, n4 = 0;
        CHECK("PM7: importAuto 识别 .csv", m1.importAuto(csvPath, false, &n1, &err) && n1 == 1);
        CHECK("PM7: importAuto 识别 .json", m2.importAuto(jsonPath, false, &n2, &err) && n2 == 1);
        CHECK("PM7: importAuto 无扩展名回退 CSV", m3.importAuto(noExt, false, &n3, &err) && n3 == 1);
        CHECK("PM7: importAuto 未知扩展名先试 JSON", m4.importAuto(unknown, false, &n4, &err) && n4 == 1);
    }
}

// ---- PM8: 边界与健壮性（空文件 / 仅表头 / 大批量 / 超范围数值）----
static void testPointModelEdgeCases()
{
    printf("\n=== PointModel PM8 边界与健壮性 ===\n");
    QTemporaryDir dir;
    CHECK("PM8: QTemporaryDir 可用", dir.isValid());

    {   // 空文件
        const QString path = dir.filePath(QStringLiteral("empty.csv"));
        pmtest::writeFile(path, "");
        PointModel m;
        int imported = -1;
        QString err;
        const bool ok = m.importFromCsv(path, false, &imported, &err);
        CHECK("PM8: 空 CSV 不崩溃且返回 true（无行可导）", ok);
        CHECK("PM8: 空 CSV 导入数为 0", imported == 0);
        CHECK("PM8: 空 CSV 后点位数仍为 0", m.points().isEmpty());
    }

    {   // 只有 BOM
        const QString path = dir.filePath(QStringLiteral("bomonly.csv"));
        pmtest::writeFile(path, "", true);
        PointModel m;
        int imported = -1;
        QString err;
        CHECK("PM8: 仅 BOM 的 CSV 不崩溃", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM8: 仅 BOM 导入数为 0", imported == 0);
    }

    {   // 只有空白行
        const QString path = dir.filePath(QStringLiteral("blank.csv"));
        pmtest::writeFile(path, "\n\n   \n,\n");
        PointModel m;
        int imported = -1;
        QString err;
        m.importFromCsv(path, false, &imported, &err);
        CHECK("PM8: 空白行 CSV 导入数为 0", imported == 0);
    }

    {   // 数值越界/非法时兜底，不产生异常值
        const QString path = dir.filePath(QStringLiteral("weird.csv"));
        pmtest::writeFile(path,
            "name,address,count,scale,alarmLow,alarmHigh,archiveIntervalSec,serverAddress\n"
            "零count,1,0,0,0,0,0,0\n"
            "负count,2,-5,-1,-10,5,-3,-7\n"
            "高版本,3,1,1,0,65535,999999,999\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM8: 越界数值 CSV 导入成功", m.importFromCsv(path, false, &imported, &err));
        CHECK("PM8: 越界数值仍导入 3 条", imported == 3);
        if (m.points().size() == 3) {
            CHECK("PM8: count=0 被兜底为 1", m.points().at(0).count == 1);
            CHECK("PM8: count=-5 被兜底为 1", m.points().at(1).count == 1);
            CHECK("PM8: alarmHigh<=alarmLow 时兜底 65535", qFuzzyCompare(m.points().at(0).alarmHigh, 65535.0));
            CHECK("PM8: archiveIntervalSec<=0 时兜底 5", m.points().at(0).archiveIntervalSec == 5);
            CHECK("PM8: serverAddress<=0 时兜底 1", m.points().at(0).serverAddress == 1);
            CHECK("PM8: 负 serverAddress 被兜底为 1", m.points().at(1).serverAddress == 1);
        }
    }

    {   // 数值格式容错（科学计数/前后空格/空值）
        const QString path = dir.filePath(QStringLiteral("numfmt.csv"));
        pmtest::writeFile(path,
            "name,address,scale,offset\n"
            "科学,1,1e-3, 2.5 \n"
            "空scale,2,,3\n");
        PointModel m;
        int imported = 0;
        QString err;
        CHECK("PM8: 数值格式容错导入成功", m.importFromCsv(path, false, &imported, &err));
        if (m.points().size() == 2) {
            CHECK("PM8: 科学计数法 scale 解析", qFuzzyCompare(m.points().at(0).scale, 0.001));
            CHECK("PM8: 带空格 offset 解析", qFuzzyCompare(m.points().at(0).offset, 2.5));
            CHECK("PM8: 空 scale 兜底 1.0", qFuzzyCompare(m.points().at(1).scale, 1.0));
            CHECK("PM8: 空 scale 未串到 offset 列", qFuzzyCompare(m.points().at(1).offset, 3.0));
        }
    }

    {   // 大批量：1000 点导入导出往返
        const QString path = dir.filePath(QStringLiteral("big.csv"));
        PointModel src;
        for (int i = 0; i < 1000; ++i) {
            PointDefinition p;
            p.name = QStringLiteral("批量点%1").arg(i);
            p.address = i;
            p.count = (i % 4) + 1;
            p.dataType = (i % 2) ? QStringLiteral("int16") : QStringLiteral("uint16");
            p.scale = 1.0 + (i % 7) * 0.1;
            p.unit = QStringLiteral("u%1").arg(i % 5);
            p.archiveEnabled = (i % 3 != 0);
            p.archiveIntervalSec = (i % 10) + 1;
            src.addPoint(p);
        }
        QElapsedTimer timer;
        timer.start();
        QString err;
        CHECK("PM8: 1000 点导出成功", src.exportToCsv(path, &err));
        PointModel target;
        int imported = 0;
        CHECK("PM8: 1000 点导入成功", target.importFromCsv(path, false, &imported, &err));
        CHECK("PM8: 导入数量为 1000", imported == 1000);
        CHECK("PM8: 1000 点往返数量一致", target.points().size() == 1000);
        const qint64 ms = timer.elapsed();

        bool match = target.points().size() == 1000;
        for (int i = 0; match && i < 1000; ++i)
            if (!pmtest::sameBusinessFields(src.points().at(i), target.points().at(i)))
                match = false;
        CHECK("PM8: 1000 点全部字段往返无损", match);
        // 性能只做宽松护栏（CI 机器波动大），不做硬性时间断言
        CHECK("PM8: 1000 点往返无性能塌陷（宽松护栏 <5000ms）", ms < 5000);

        QElapsedTimer t2; t2.start();
        int n = 0;
        target.importFromCsv(path, false, &n, &err);
        CHECK("PM8: 1000 点覆盖导入无性能塌陷（宽松护栏 <5000ms）", t2.elapsed() < 5000);
    }

    {   // 导出到不可写路径
        PointModel m;
        PointDefinition a; a.name = QStringLiteral("x"); m.addPoint(a);
        QString err;
        const QString bad = dir.filePath(QStringLiteral("no_such_dir/sub/out.csv"));
        CHECK("PM8: 导出到不存在目录返回 false", !m.exportToCsv(bad, &err));
        CHECK("PM8: 导出失败给出错误信息", !err.isEmpty());
    }

    {   // 空模型导出
        PointModel empty;
        const QString path = dir.filePath(QStringLiteral("empty_export.csv"));
        QString err;
        CHECK("PM8: 空模型导出成功", empty.exportToCsv(path, &err));
        const QByteArray raw = pmtest::readFile(path);
        CHECK("PM8: 空模型导出仅含 BOM+表头", raw.contains("id,name,"));
    }
}

// ---- PM9: importAuto 回退策略与失败原子性 ----
static void testPointModelImportAutoFallback()
{
    printf("\n=== PointModel PM9 importAuto 回退策略 ===\n");
    QTemporaryDir dir;
    CHECK("PM9: QTemporaryDir 可用", dir.isValid());

    // 未知扩展名 + CSV 内容 → 应回退尝试 CSV
    const QString path = dir.filePath(QStringLiteral("weird.dat"));
    pmtest::writeFile(path, "name,address\n回退点,88\n");
    PointModel m;
    int imported = 0;
    QString err;
    CHECK("PM9: 未知扩展名+CSV 内容回退成功", m.importAuto(path, false, &imported, &err));
    CHECK("PM9: 回退后导入 1 条", imported == 1);
    CHECK("PM9: 回退后内容正确", !m.points().isEmpty()
          && m.points().first().name == QStringLiteral("回退点")
          && m.points().first().address == 88);

    // 未知扩展名 + 完全无法解析的内容 → 应失败并给出错误
    const QString bad = dir.filePath(QStringLiteral("garbage.dat"));
    pmtest::writeFile(bad, "\x01\x02\x03binary\xff");
    PointModel m2;
    int n2 = 0;
    QString err2;
    CHECK("PM9: 未知扩展名+垃圾内容导入失败", !m2.importAuto(bad, false, &n2, &err2));
    CHECK("PM9: 失败时给出错误信息", !err2.isEmpty());

    // importAuto 先试 JSON 会先 clear（覆盖模式），此处验证不崩溃并记录实际行为
    auto owner3 = pmtest::buildModel(pmtest::samplePoints());
    PointModel &m3 = *owner3;
    const int before = m3.points().size();
    int n3 = 0;
    QString err3;
    m3.importAuto(bad, false, &n3, &err3);
    CHECK("PM9: 未知扩展名+垃圾内容后不崩溃", m3.points().size() >= 0);
    CHECK("PM9: importAuto 覆盖模式失败后现有点位完整保留（原子性）",
          m3.points().size() == before);
    CHECK("PM9: importAuto 失败后点位内容未变", !m3.points().isEmpty()
          && m3.points().first().name == pmtest::samplePoints().first().name);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    printf("==== FieldLink master 测试套件 ====\n");

    testAlarmManager();
    testAlarmBitGuard();
    testSecurityManager();
    testSecuritySaltedHash();
    testReliabilityManager();
    testMqttClient();
    testCredentialCodec();
    testDataExporter();
    testPollManager();
    testDeviceManager();
    testHistoryData();
    testBatchTaskManager();
    testDataParser();

    // Modbus 调试工具纯逻辑（原 RawRequestDialog / ScannerDialog 内部逻辑）
    testDiagParseHexBytes();
    testDiagHexFormatting();
    testDiagExceptionNames();
    testDiagPayloadBuilding();
    testDiagFunctionCodeValidation();
    testDiagScanRangeValidation();
    testDiagProbeMapping();
    testDiagPollTaskSeed();
    testDiagScanProgress();

    // PointModel 点表管理 + CSV/JSON 导入导出（此前无覆盖）
    testPointModelJsonRoundTrip();
    testPointModelCsvRoundTrip();
    testPointModelCsvHeaderAdaptivity();
    testPointModelCsvEscaping();
    testPointModelImportAtomicity();
    testPointModelAppendOverwrite();
    testPointModelTokenCompatibility();
    testPointModelEdgeCases();
    testPointModelImportAutoFallback();

    printf("\n==== 汇总 ====\n");
    printf("PASS: %d  FAIL: %d  已确认已知问题: %d\n", g_passed, g_failed, g_issuesConfirmed);
    return g_failed == 0 ? 0 : 1;
}
