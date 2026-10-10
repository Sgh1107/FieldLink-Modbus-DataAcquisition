// mqtt_e2e.cpp —— MQTT 端到端集成测试（真实 broker + 真实客户端）
//
// 与 master_tests.cpp 里 MqttClient 单元测试的区别：
//   单元测试：进程内 FakeBroker（自实现的迷你 broker），验证协议编解码细节
//   本工具：  连接**外部真实 broker**（由 tools/mqtt_test.sh 用 python 起），
//             验证真实 socket 路径、认证、QoS、主题、payload、断线重连
//
// 设计取舍：broker 由脚本外部启动，而非本程序用 QProcess 拉起。
//   原因有二：
//     1)部分受限环境（IDE 内 / 沙箱）下 QProcess 启动子进程会被系统拒绝，
//        表现为「没有可用的应用程序」而与代码无关；
//     2) broker 本就是独立外部服务，分离更贴近真实部署，也便于手工联调。
//
// 用法：
//   mqtt_e2e --port 18885                     # 连接已运行的 broker
//   mqtt_e2e --port 18891 --user u --pass p    # 带认证
//
// 退出码：0 = 全部通过，1 = 有失败

#include <QCoreApplication>
#include <QTcpSocket>
#include <QTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QCommandLineParser>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

#include "mqttclient.h"

// Windows 控制台默认代码页（GBK/936）无法输出 UTF-8 中文，printf 的中文会乱码。
// SetConsoleOutputCP 只对真实控制台有效，stdout 被重定向到管道时无效
// （脚本/CI 里正是这种情况），故这里统一走 UTF-8 字节直写。
static void enableUtf8Console()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// 以 UTF-8 字节输出到 stdout，绕过 CRT 的代码页转换
static void outUtf8(const QString &text)
{
#ifdef _WIN32
    const QByteArray bytes = text.toUtf8();
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(h, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    fflush(stdout);
#else
    fputs(text.toUtf8().constData(), stdout);
#endif
}

static int g_pass = 0;
static int g_fail = 0;

static void check(const QString &name, bool ok, const QString &detail = QString())
{
    if (ok) {
        outUtf8(QStringLiteral("  [PASS] ") + name + QStringLiteral("\n"));
        ++g_pass;
    } else {
        outUtf8(QStringLiteral("  [FAIL] ") + name
                + (detail.isEmpty() ? QString()
                                    : QStringLiteral("  -> ") + detail)
                + QStringLiteral("\n"));
        ++g_fail;
    }
}

static void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 等待 broker 端口就绪（TCP 可达即可）
static bool waitForPort(quint16 port, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, port);
        if (probe.waitForConnected(300)) {
            probe.disconnectFromHost();
            return true;
        }
        waitMs(200);
    }
    return false;
}

// ============ E2E-1 连接与 QoS0 发布 ============
static void testConnectAndPublish(quint16 port)
{
    outUtf8(QStringLiteral("\n=== E2E-1 连接 / QoS0 发布 / 主题与载荷 ===\n"));

    MqttClient client;
    bool connected = false;
    QString err;
    QObject::connect(&client, &MqttClient::connected, [&]() { connected = true; });
    QObject::connect(&client, &MqttClient::errorOccurred,
                     [&](const QString &m) { err = m; });

    client.setBroker("127.0.0.1", port);
    // clientId 必须唯一：broker 以它标识会话，重复 id 会顶掉旧会话
    client.setCredentials(QStringLiteral("e2e-qos0"));
    client.setAutoReconnect(false);
    client.setPublishQos(0);
    client.connectToBroker();

    QElapsedTimer t;
    t.start();
    while (!connected && t.elapsed() < 10000)
        waitMs(100);
    if (!connected) waitMs(300);   // 留出errorOccurred 到达的时间

    check("E2E-1: 完成 CONNECT/CONNACK 握手", connected, err);
    check("E2E-1: isConnectedToBroker() 状态一致",
          client.isConnectedToBroker() == connected);
    check("E2E-1: brokerInfo() 含正确端口",
          client.brokerInfo().contains(QString::number(port)), client.brokerInfo());
    if (!connected)
        return;

    int bytes = -1;
    QString topic;
    QObject::connect(&client, &MqttClient::published,
                     [&](const QString &tp, int b) { topic = tp; bytes = b; });

    const QByteArray payload = QJsonDocument(QJsonObject{
        {"timestamp", "2026-10-10T10:00:00.000"},
        {"value", 42}
    }).toJson(QJsonDocument::Compact);

    check("E2E-1: publish() 返回 true",
          client.publish(QStringLiteral("fieldlink/e2e/qos0"), payload));
    QElapsedTimer t2; t2.start();
    while (bytes < 0 && t2.elapsed() < 3000) waitMs(50);
    check("E2E-1: published 信号按预期发出",
          bytes == payload.size(),
          QStringLiteral("期望 %1 实际 %2").arg(payload.size()).arg(bytes));
    check("E2E-1: published 携带正确主题",
          topic == QStringLiteral("fieldlink/e2e/qos0"), topic);

    check("E2E-1: retain 发布被接受",
          client.publish(QStringLiteral("fieldlink/e2e/retain"), QByteArray("r"), true));
    waitMs(300);

    // JSON 发布
    QObject::disconnect(&client, &MqttClient::published, nullptr, nullptr);
    bytes = -1;
    QObject::connect(&client, &MqttClient::published,
                     [&](const QString &, int b) { bytes = b; });
    QJsonObject obj;
    obj["device"] = QStringLiteral("PLC-1");
    obj["value"] = 123.45;
    check("E2E-1: publishJson() 返回 true",
          client.publishJson(QStringLiteral("fieldlink/e2e/json"), obj));
    waitMs(500);
    check("E2E-1: publishJson 载荷非空", bytes > 0, QStringLiteral("bytes=%1").arg(bytes));

    client.disconnectFromBroker();
    waitMs(200);
    check("E2E-1: 主动断开后状态为未连接", !client.isConnectedToBroker());
}

// ============ E2E-2 QoS 1 ============
static void testQos1(quint16 port)
{
    outUtf8(QStringLiteral("\n=== E2E-2 QoS 1（等待 PUBACK 确认）===\n"));

    MqttClient client;
    bool connected = false;
    QObject::connect(&client, &MqttClient::connected, [&]() { connected = true; });
    client.setBroker("127.0.0.1", port);
    client.setCredentials(QStringLiteral("e2e-qos1"));
    client.setAutoReconnect(false);
    client.setPublishQos(1);
    client.connectToBroker();

    QElapsedTimer t; t.start();
    while (!connected && t.elapsed() < 10000) waitMs(100);
    check("E2E-2: QoS1 模式连接成功", connected);
    if (!connected) return;

    int bytes = -1;
    QObject::connect(&client, &MqttClient::published,
                     [&](const QString &, int b) { bytes = b; });

    const QByteArray p = QByteArray("qos1-e2e-payload");
    check("E2E-2: QoS1 publish() 返回 true",
          client.publish(QStringLiteral("fieldlink/e2e/qos1"), p));

    QElapsedTimer t2; t2.start();
    while (bytes < 0 && t2.elapsed() < 6000) waitMs(50);
    check("E2E-2: 收到 PUBACK 后才发 published（QoS1 闭环）",
          bytes == p.size(),
          QStringLiteral("期望 %1 实际 %2").arg(p.size()).arg(bytes));

    client.disconnectFromBroker();
    waitMs(200);
}

// ============ E2E-3 认证 ============
static void testAuth(quint16 port, const QString &user, const QString &pass)
{
    outUtf8(QStringLiteral("\n=== E2E-3 认证 ===\n"));
    if (user.isEmpty()) {
        outUtf8(QStringLiteral("  [SKIP] broker 未开启认证（加 --user/--pass 参数可测）\n"));
        return;
    }

    {   // 错误凭据
        MqttClient bad;
        bool connected = false;
        QString err;
        QObject::connect(&bad, &MqttClient::connected, [&]() { connected = true; });
        QObject::connect(&bad, &MqttClient::errorOccurred,
                         [&](const QString &m) { err = m; });
        bad.setBroker("127.0.0.1", port);
        bad.setCredentials(QStringLiteral("wrong"), QStringLiteral("bad"), QStringLiteral("bad"));
        bad.setAutoReconnect(false);
        bad.connectToBroker();
        QElapsedTimer t; t.start();
        while (!connected && err.isEmpty() && t.elapsed() < 10000) waitMs(100);
        waitMs(500);
        check("E2E-3: 错误凭据未进入连接态", !connected);
        check("E2E-3: 错误凭据产生明确提示", !err.isEmpty(), err);
    }

    {   // 正确凭据（对照，证明 broker 认证本身正常）
        MqttClient good;
        bool connected = false;
        QObject::connect(&good, &MqttClient::connected, [&]() { connected = true; });
        good.setBroker("127.0.0.1", port);
        good.setCredentials(QStringLiteral("good-client"), user, pass);
        good.setAutoReconnect(false);
        good.connectToBroker();
        QElapsedTimer t; t.start();
        while (!connected && t.elapsed() < 10000) waitMs(100);
        check("E2E-3: 正确凭据可连接（对照验证）", connected);
        good.disconnectFromBroker();
        waitMs(200);
    }
}

// ============ E2E-4 连接状态与用户意图门控 ============
static void testReconnect(quint16 port)
{
    outUtf8(QStringLiteral("\n=== E2E-4 连接状态与用户意图门控 ===\n"));

    MqttClient client;
    int connectCount = 0;
    QObject::connect(&client, &MqttClient::connected, [&]() { ++connectCount; });

    client.setBroker("127.0.0.1", port);
    client.setCredentials(QStringLiteral("e2e-reconnect"));
    client.setAutoReconnect(true);
    client.setKeepAlive(10);
    client.connectToBroker();

    QElapsedTimer t; t.start();
    while (connectCount == 0 && t.elapsed() < 10000) waitMs(100);
    check("E2E-4: 自动重连开启时能连上", connectCount > 0);
    if (connectCount == 0) return;

    // 主动断开 → 不应再自动重连（用户意图门控，这是历史修复过的点）
    const int before = connectCount;
    client.disconnectFromBroker();
    waitMs(3000);
    check("E2E-4: 主动断开后不再重连（用户意图门控）",
          connectCount == before,
          QStringLiteral("连接次数 %1 -> %2").arg(before).arg(connectCount));
    check("E2E-4: 主动断开后状态为未连接", !client.isConnectedToBroker());
}

// ============ E2E-5 未连接保护 ============
static void testPublishWhileDisconnected()
{
    outUtf8(QStringLiteral("\n=== E2E-5 未连接状态保护 ===\n"));

    MqttClient client;
    client.setBroker("127.0.0.1", 1);   // 必然连不通
    client.setAutoReconnect(false);

    int errorCount = 0;
    QObject::connect(&client, &MqttClient::errorOccurred,
                     [&](const QString &) { ++errorCount; });

    check("E2E-5: 未连接时 publish() 返回 false（不崩溃）",
          !client.publish(QStringLiteral("fieldlink/e2e/never"), QByteArray("x")));
    check("E2E-5: 未连接时未进入连接态", !client.isConnectedToBroker());

    // 连续多次未连接发布：告警应合并，不刷屏
    const int before = errorCount;
    for (int i = 0; i < 5; ++i)
        client.publish(QStringLiteral("fieldlink/e2e/never"), QByteArray("x"));
    waitMs(400);
    check("E2E-5: 连续未连接发布不刷屏（告警合并）",
          errorCount - before <= 1,
          QStringLiteral("新增告警 %1 条").arg(errorCount - before));
}

// ============ E2E-6 载荷边界 ============
static void testPayloadEdges(quint16 port)
{
    outUtf8(QStringLiteral("\n=== E2E-6 载荷边界与编码 ===\n"));

    MqttClient client;
    bool connected = false;
    QObject::connect(&client, &MqttClient::connected, [&]() { connected = true; });
    client.setBroker("127.0.0.1", port);
    client.setCredentials(QStringLiteral("e2e-payload"));
    client.setAutoReconnect(false);
    client.connectToBroker();
    QElapsedTimer t; t.start();
    while (!connected && t.elapsed() < 10000) waitMs(100);
    if (!connected) {
        check("E2E-6: 连接 broker", false);
        return;
    }

    int bytes = -1;
    QObject::connect(&client, &MqttClient::published,
                     [&](const QString &, int b) { bytes = b; });

    check("E2E-6: 空载荷 publish 被接受",
          client.publish(QStringLiteral("fieldlink/e2e/empty"), QByteArray()));
    waitMs(400);

    // 较大载荷（8KB → Remaining Length 需多字节变长编码）
    const QByteArray big(8192, 'A');
    bytes = -1;
    check("E2E-6: 8KB 载荷 publish 被接受",
          client.publish(QStringLiteral("fieldlink/e2e/big"), big));
    QElapsedTimer t2; t2.start();
    while (bytes < 0 && t2.elapsed() < 5000) waitMs(50);
    check("E2E-6: 8KB 载荷完整发出（多字节长度字段）",
          bytes == big.size(),
          QStringLiteral("期望 %1 实际 %2").arg(big.size()).arg(bytes));

    // UTF-8 中文主题
    check("E2E-6: UTF-8 中文主题被接受",
          client.publish(QString::fromUtf8("fieldlink/e2e/温度/1"), QByteArray("t")));
    waitMs(400);

    // 二进制载荷（含 0x00 等非文本字节）
    QByteArray bin;
    bin.append(char(0x00)); bin.append(char(0xFF)); bin.append(char(0x10));
    check("E2E-6: 含 0x00/0xFF 的二进制载荷被接受",
          client.publish(QStringLiteral("fieldlink/e2e/binary"), bin));
    waitMs(400);

    client.disconnectFromBroker();
    waitMs(200);
}

int main(int argc, char *argv[])
{
    enableUtf8Console();
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("FieldLink MQTT end-to-end test (client side)");
    parser.addHelpOption();
    QCommandLineOption portOpt(QStringLiteral("port"), QStringLiteral("broker port"), QStringLiteral("port"), QStringLiteral("1883"));
    QCommandLineOption userOpt(QStringLiteral("user"), QStringLiteral("broker username"), QStringLiteral("user"));
    QCommandLineOption passOpt(QStringLiteral("pass"), QStringLiteral("broker password"), QStringLiteral("pass"));
    parser.addOption(portOpt); parser.addOption(userOpt); parser.addOption(passOpt);
    parser.process(app);

    const quint16 port = static_cast<quint16>(parser.value(portOpt).toUShort());
    const QString user = parser.value(userOpt);
    const QString pass = parser.value(passOpt);

    outUtf8(QStringLiteral("==== FieldLink MQTT 端到端测试（客户端侧）====\n"));
    outUtf8(QStringLiteral("broker: 127.0.0.1:%1%2\n").arg(port)
            .arg(user.isEmpty() ? QString() : QStringLiteral("  (auth: %1)").arg(user)));

    if (!waitForPort(port, 3000)) {
        outUtf8(QStringLiteral("  [FAIL] 无法连接 broker 127.0.0.1:%1\n").arg(port));
        outUtf8(QStringLiteral("         请先启动：python slave/mqtt_test_broker.py --port %1\n").arg(port));
        outUtf8(QStringLiteral("         或直接运行：bash tools/mqtt_test.sh（脚本会自动拉起 broker）\n"));
        return 1;
    }
    outUtf8(QStringLiteral("broker 就绪\n"));

    testConnectAndPublish(port);
    testQos1(port);
    testAuth(port, user, pass);
    testReconnect(port);
    testPublishWhileDisconnected();
    testPayloadEdges(port);

    printf("\n==== 汇总 ====\n");
    printf("PASS: %d  FAIL: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}