// test_security_config.cpp
// 【安全与配置持久化】模块的回归断言（对应本轮修复的缺陷清单）。
//
// 覆盖：
//   P2-5  ConfigProfile 目录穿越（档案名未过滤）
//   P2-6  ConfigProfile 恒返回 true（未检查 QSettings::status）
//   P2-3  DeviceManager 加载缺键退化为 0/false（R-DEVCFG-1）
//   P2-4  DeviceManager 加载非原子（中途失败留下残缺状态）
//   P2-11 DeviceManager updateDevice 不重建连接
//   P2-12 DeviceManager stateChanged 回调归属错乱
//   P2-38 口令哈希为单次 SHA-256
//   P2-39 鉴权非常量时间比较
//   P2-40 login 无失败锁定/限速 + 审计目录不可写
//   P2-41 强制改密只在 UI 层强制
//   P2-42 审计日志可注入伪造 + 无轮转
//   P2-43 admin 角色/启用状态可被改掉
//   P2-44 ensureDefaults 只在整体为空时建 admin
//   P2-45 用户名未归一化
//   P2-46 CredentialCodec 硬编码 XOR 密钥
//   P2-47 CredentialCodec Base64 静默失败
//
// 每条断言都写成「修复前必然失败、修复后必然通过」的形式。

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QTcpServer>
#include <cstdio>

#include "securitymanager.h"
#include "configprofile.h"
#include "devicemanager.h"
#include "credentialcodec.h"

// 静态计数器（定义在本文件内）
static int g_secPassed = 0;
static int g_secFailed = 0;

// name 允许传 const char* 字面量或 QString；统一转 UTF-8 字节再打印，
// 避免把 QString 直接传给 printf 的 %s（类型不匹配告警）。
#define SEC_CHECK(name, cond) do { \
    const QByteArray _name = QString(name).toUtf8(); \
    if ((cond)) { printf("  [PASS] %s\n", _name.constData()); ++g_secPassed; } \
    else { printf("  [FAIL] %s (%s:%d)\n", _name.constData(), __FILE__, __LINE__); ++g_secFailed; } \
} while (0)

// 供主控汇总使用（与上面的静态计数器同源）
int securityConfigPassed();
int securityConfigFailed();

// ---------------- 小工具 ----------------

static QString sha256Hex(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

static void secWaitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 统计文件中的完整行数（按 '\n' 计）
static int secCountLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;
    const QString content = QString::fromUtf8(f.readAll());
    f.close();
    int lines = 0;
    for (const QChar &c : content) {
        if (c == QLatin1Char('\n'))
            ++lines;
    }
    return lines;
}

// 载入一次空配置 → 建立默认角色（admin/engineer/operator/viewer）与默认 admin 账号
static void secInitRoles(SecurityManager &sm)
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "roles");
    s.clear();
    sm.load(s);
}

static QString secTodayLogPath(const SecurityManager &sm)
{
    return sm.auditLogPath() + QLatin1Char('/')
           + QDate::currentDate().toString("yyyyMMdd") + QStringLiteral(".audit.log");
}

// =====================================================================
// P2-5 / P2-6：ConfigProfile 目录穿越 + 状态检查
// =====================================================================
static void secTestConfigProfile()
{
    printf("\n=== ConfigProfile（P2-5 目录穿越 / P2-6 状态检查）===\n");

    QTemporaryDir dir;
    ConfigProfileManager cp;
    cp.setProfileDirectory(dir.path());

    QSettings src(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "profile");
    src.setValue("demo/key", 42);

    // ---- P2-5：目录穿越 ----
    const QString outside = QDir(dir.path()).absoluteFilePath("../../escaped.ini");
    QFile::remove(outside);
    SEC_CHECK("P2-5: saveProfile(\"../../escaped\") 被拒绝", !cp.saveProfile("../../escaped", src));
    SEC_CHECK("P2-5: 目录穿越未在档案目录之外创建文件", !QFile::exists(outside));
    SEC_CHECK("P2-5: loadProfile 对穿越名返回 false", !cp.loadProfile("../../escaped", src));
    SEC_CHECK("P2-5: profileExists 对穿越名返回 false", !cp.profileExists("../../escaped"));
    SEC_CHECK("P2-5: deleteProfile 对穿越名返回 false", !cp.deleteProfile("../../escaped"));

    SEC_CHECK("P2-5: 含路径分隔符的名字被拒绝", !cp.saveProfile("sub/dir", src));
    SEC_CHECK("P2-5: 含非法字符的名字被拒绝", !cp.saveProfile("a:b*c?d", src));
    SEC_CHECK("P2-5: 含 .. 的名字被拒绝", !cp.saveProfile("..", src));
    SEC_CHECK("P2-5: 空名字被拒绝", !cp.saveProfile("   ", src));
    SEC_CHECK("P2-5: 非法名不会在档案目录里留下文件",
              !QFile::exists(dir.path() + "/escaped.ini")
              && !QFile::exists(dir.path() + "/..ini"));

    // ---- 合法名字仍然可用 ----
    SEC_CHECK("P2-5: 合法名字可保存", cp.saveProfile("good", src));
    SEC_CHECK("P2-5: 合法档案文件已生成", QFile::exists(dir.path() + "/good.ini"));
    SEC_CHECK("P2-5: 合法档案 profileExists 为 true", cp.profileExists("good"));
    SEC_CHECK("P2-5: 改名到非法名被拒绝", !cp.renameProfile("good", "../evil"));
    SEC_CHECK("P2-5: 拒绝改名后原档案仍在", QFile::exists(dir.path() + "/good.ini"));
    SEC_CHECK("P2-5: 改名到合法名成功", cp.renameProfile("good", "good2"));
    SEC_CHECK("P2-5: 改名后新档案存在", QFile::exists(dir.path() + "/good2.ini"));

    // ---- P2-6：不可写位置必须返回 false（而不是恒 true）----
    {
        QTemporaryDir dir2;
        QFile blocker(dir2.path() + "/blocker");
        blocker.open(QIODevice::WriteOnly);
        blocker.write("not-a-directory");
        blocker.close();

        ConfigProfileManager cp2;
        cp2.setProfileDirectory(dir2.path() + "/blocker/sub");   // 父级是文件 → 目录建不出来
        QSettings src2(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "profile2");
        src2.setValue("k", 1);
        SEC_CHECK("P2-6: 不可写位置保存返回 false", !cp2.saveProfile("p1", src2));
    }

    // ---- P2-6：损坏档案（同名目录）加载必须失败 ----
    {
        QTemporaryDir dir3;
        ConfigProfileManager cp3;
        cp3.setProfileDirectory(dir3.path());
        QDir(dir3.path()).mkpath("broken.ini");    // 同名目录 → QSettings 打不开
        QSettings target(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "profile3");
        SEC_CHECK("P2-6: 损坏档案加载返回 false", !cp3.loadProfile("broken", target));
    }

    // ---- P2-6：合并语义（不是替换）----
    {
        QTemporaryDir dir4;
        ConfigProfileManager cp4;
        cp4.setProfileDirectory(dir4.path());
        QSettings profileSrc(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "profile4");
        profileSrc.setValue("only/in/profile", "v");
        cp4.saveProfile("merge", profileSrc);

        QSettings target2(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "target4");
        target2.setValue("keep/me", "untouched");
        target2.setValue("only/in/profile", "old");
        SEC_CHECK("P2-6: 合并语义加载成功", cp4.loadProfile("merge", target2));
        SEC_CHECK("P2-6: 合并后档案中的键覆盖目标值",
                  target2.value("only/in/profile").toString() == "v");
        SEC_CHECK("P2-6: 合并后档案未覆盖的键保留原值",
                  target2.value("keep/me").toString() == "untouched");
    }
}

// =====================================================================
// P2-3 / P2-4 / P2-11 / P2-12：DeviceManager
// =====================================================================
static void secTestDeviceManager()
{
    printf("\n=== DeviceManager（P2-3 缺键默认值 / P2-4 非原子 / P2-11 重建 / P2-12 信号归属）===\n");

    // ---- P2-3：缺键时必须使用与 DeviceConfig 一致的默认值 ----
    {
        QSettings s(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "dev1");
        s.clear();
        s.setValue("devices/size", 1);
        s.setValue("devices/1/name", "D1");
        s.setValue("devices/1/portOrAddress", "127.0.0.1:502");
        // 故意不写 isTcp / baud / dataBits / stopBits / serverAddress
        DeviceManager dm;
        dm.loadFromSettings(s);
        const DeviceConfig c = dm.deviceConfig(1);
        SEC_CHECK("P2-3: 缺键时仍为 TCP 设备（不再静默变串口）", c.isTcp);
        SEC_CHECK("P2-3: 缺键时波特率回落 9600", c.baud == 9600);
        SEC_CHECK("P2-3: 缺键时数据位回落 8", c.dataBits == 8);
        SEC_CHECK("P2-3: 缺键时停止位回落 1", c.stopBits == 1);
        SEC_CHECK("P2-3: 缺键时从站地址回落 1", c.serverAddress == 1);
        SEC_CHECK("P2-3: 缺键时响应超时回落 1000", c.responseTime == 1000);
        SEC_CHECK("P2-3: 缺键时重试次数回落 3", c.numberOfRetries == 3);
        SEC_CHECK("P2-3: 缺键时轮询间隔回落 1000", c.pollIntervalMs == 1000);
    }

    // ---- P2-3：越界值回落默认值 ----
    {
        QSettings s(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "dev2");
        s.clear();
        s.setValue("devices/size", 2);
        s.setValue("devices/1/name", "bad");
        s.setValue("devices/1/baud", 5);
        s.setValue("devices/1/dataBits", 99);
        s.setValue("devices/1/stopBits", 0);
        s.setValue("devices/1/serverAddress", 500);
        s.setValue("devices/1/pollIntervalMs", 10);
        s.setValue("devices/1/responseTime", 999999);
        s.setValue("devices/2/name", "ok");
        s.setValue("devices/2/baud", 19200);
        DeviceManager dm;
        dm.loadFromSettings(s);
        const DeviceConfig bad = dm.deviceConfig(1);
        SEC_CHECK("P2-3: 越界波特率回落 9600", bad.baud == 9600);
        SEC_CHECK("P2-3: 越界数据位回落 8", bad.dataBits == 8);
        SEC_CHECK("P2-3: 越界停止位回落 1", bad.stopBits == 1);
        SEC_CHECK("P2-3: 越界从站地址回落 1", bad.serverAddress == 1);
        SEC_CHECK("P2-3: 过小轮询间隔回落 1000", bad.pollIntervalMs == 1000);
        SEC_CHECK("P2-3: 超大响应超时回落 1000", bad.responseTime == 1000);
        SEC_CHECK("P2-3: 合法自定义波特率被保留", dm.deviceConfig(2).baud == 19200);
        SEC_CHECK("P2-3: 越界条目不影响其它条目（整批载入）", dm.allDevices().size() == 2);
    }

    // ---- P2-4：加载必须是整体替换（先解析后替换，不留残缺状态）----
    {
        QSettings s(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "dev3");
        s.clear();
        s.setValue("devices/size", 3);
        for (int i = 1; i <= 3; ++i) {
            s.setValue(QString("devices/%1/name").arg(i), QString("D%1").arg(i));
            s.setValue(QString("devices/%1/portOrAddress").arg(i), "127.0.0.1:502");
        }
        DeviceManager dm;
        dm.loadFromSettings(s);
        SEC_CHECK("P2-4: 首次加载得到 3 个设备", dm.allDevices().size() == 3);

        QSettings s2(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "dev3b");
        s2.clear();
        s2.setValue("devices/size", 1);
        s2.setValue("devices/1/name", "only");
        dm.loadFromSettings(s2);
        SEC_CHECK("P2-4: 重新加载后设备集合被整体替换（无旧设备残留）",
                  dm.allDevices().size() == 1);
        SEC_CHECK("P2-4: 重新加载后内容为新集合", dm.deviceConfig(1).name == "only");

        QSettings s3(QSettings::IniFormat, QSettings::UserScope, "FieldLinkSecTest", "dev3c");
        s3.clear();
        dm.loadFromSettings(s3);
        SEC_CHECK("P2-4: 空配置加载后设备集合为空（不留残缺状态）",
                  dm.allDevices().isEmpty());
    }

    // ---- P2-11：连接参数变化必须重建 client ----
    {
        DeviceManager dm;
        DeviceConfig cfg;
        cfg.name = "serial";
        cfg.isTcp = false;
        cfg.portOrAddress = "COM77";        // 不存在的串口：连接必然失败，但 client 已创建
        cfg.baud = 9600; cfg.dataBits = 8; cfg.stopBits = 1;
        cfg.serverAddress = 1; cfg.responseTime = 1000;
        cfg.numberOfRetries = 3; cfg.pollIntervalMs = 1000;
        const int id = dm.addDevice(cfg);
        dm.connectDevice(id);
        SEC_CHECK("P2-11: 连接尝试后存在 client", dm.modbusClient(id) != nullptr);

        DeviceConfig upd = dm.deviceConfig(id);
        upd.baud = 19200;                    // 仅改波特率 → 属于连接参数
        dm.updateDevice(upd);
        QModbusClient *after = dm.modbusClient(id);
        SEC_CHECK("P2-11: 连接参数变化后 client 被重建", after != nullptr);
        SEC_CHECK("P2-11: 重建后的 client 使用新波特率",
                  after != nullptr
                  && after->connectionParameter(QModbusDevice::SerialBaudRateParameter).toInt() == 19200);

        DeviceConfig upd2 = dm.deviceConfig(id);
        upd2.name = "renamed";               // 仅改名称 → 非连接参数
        dm.updateDevice(upd2);
        SEC_CHECK("P2-11: 仅改名称时配置更新生效", dm.deviceConfig(id).name == "renamed");
        SEC_CHECK("P2-11: 仅改名称时 client 仍按新参数工作",
                  dm.modbusClient(id) != nullptr
                  && dm.modbusClient(id)->connectionParameter(QModbusDevice::SerialBaudRateParameter).toInt() == 19200);
    }

    // ---- P2-12：重建 client 时旧 client 的断开信号必须被忽略 ----
    {
        QTcpServer server;
        server.listen(QHostAddress::LocalHost);

        DeviceManager dm;
        DeviceConfig cfg;
        cfg.name = "tcp";
        cfg.isTcp = true;
        cfg.portOrAddress = QString("127.0.0.1:%1").arg(server.serverPort());
        const int id = dm.addDevice(cfg);

        int disconnected = 0;
        int connected = 0;
        QObject::connect(&dm, &DeviceManager::deviceDisconnected, [&](int) { ++disconnected; });
        QObject::connect(&dm, &DeviceManager::deviceConnected, [&](int) { ++connected; });

        dm.connectDevice(id);
        secWaitMs(400);
        SEC_CHECK("P2-12: 首次连接成功", dm.isConnected(id));

        const int d0 = disconnected;
        const int c0 = connected;
        dm.connectDevice(id);               // 重建 client（旧 client 被断开）
        secWaitMs(400);
        SEC_CHECK("P2-12: 重建 client 时旧 client 的断开信号被忽略（无虚假断开）",
                  disconnected == d0);
        SEC_CHECK("P2-12: 重建后重新建立连接", dm.isConnected(id) && connected > c0);
    }
}

// =====================================================================
// P2-38 ~ P2-45：SecurityManager
// =====================================================================
static void secTestSecurityManager()
{
    printf("\n=== SecurityManager（P2-38 ~ P2-45）===\n");

    // ---- P2-38：新写入的凭据必须使用 PBKDF2（v2）----
    {
        SecurityManager sm;
        sm.addOrUpdateUser("u1", "pw123", "operator", true);
        QString hash, salt;
        for (const auto &u : sm.users()) {
            if (u.username == "u1") { hash = u.passwordHash; salt = u.passwordSalt; }
        }
        SEC_CHECK("P2-38: 口令哈希使用 v2(PBKDF2) 格式", hash.startsWith("v2:"));
        SEC_CHECK("P2-38: 口令哈希不再是单次 SHA256(\"v1:盐:口令\")",
                  hash != sha256Hex(QString("v1:%1:pw123").arg(salt)));

        SecurityManager sm2;
        sm2.addOrUpdateUser("u1", "pw123", "operator", true);
        QString hash2, salt2;
        for (const auto &u : sm2.users()) {
            if (u.username == "u1") { hash2 = u.passwordHash; salt2 = u.passwordSalt; }
        }
        SEC_CHECK("P2-38: 相同口令两次哈希结果不同（随机盐）",
                  hash != hash2 && salt != salt2);
        SEC_CHECK("P2-38: v2 口令可登录", sm.login("u1", "pw123"));
        SEC_CHECK("P2-38: 错误口令被拒绝", !sm.login("u1", "wrong"));
        sm.logout();
    }

    // ---- P2-38：旧格式凭据仍要能校验（向后兼容）----
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + "/legacy.ini";
        {
            QSettings w(ini, QSettings::IniFormat);
            w.setValue("security/users", QStringList() << "legacyv1" << "legacyraw");
            w.setValue("security/user/legacyv1/passwordSalt", "salt-1");
            w.setValue("security/user/legacyv1/passwordHash", sha256Hex("v1:salt-1:oldpw"));
            w.setValue("security/user/legacyv1/role", "operator");
            w.setValue("security/user/legacyv1/enabled", true);
            w.setValue("security/user/legacyraw/passwordHash", sha256Hex("pwd:rawpw"));
            w.setValue("security/user/legacyraw/role", "operator");
            w.setValue("security/user/legacyraw/enabled", true);
        }
        SecurityManager sm;
        QSettings r(ini, QSettings::IniFormat);
        sm.load(r);
        SEC_CHECK("P2-38: 旧 v1 加盐口令仍可登录", sm.login("legacyv1", "oldpw"));
        sm.logout();
        SEC_CHECK("P2-38: 旧无盐裸 SHA256 口令仍可登录", sm.login("legacyraw", "rawpw"));
        sm.logout();
    }

    // ---- P2-38：旧格式 Token 仍要能校验 + 新 Token 持久化回环 ----
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + "/legacy-token.ini";
        {
            QSettings w(ini, QSettings::IniFormat);
            w.setValue("security/apiTokenHash", "tok-salt:" + sha256Hex("v1:tok-salt:mytoken"));
        }
        SecurityManager sm;
        QSettings r(ini, QSettings::IniFormat);
        sm.load(r);
        SEC_CHECK("P2-38: 旧 v1 Token 仍可校验", sm.verifyApiToken("mytoken"));
        SEC_CHECK("P2-38: 旧 v1 Token 错误口令被拒绝", !sm.verifyApiToken("nope"));
    }
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + "/token.ini";
        SecurityManager sm;
        QSettings w(ini, QSettings::IniFormat);
        sm.setApiToken("round-trip");
        sm.save(w);
        w.sync();
        SecurityManager sm2;
        QSettings r(ini, QSettings::IniFormat);
        sm2.load(r);
        SEC_CHECK("P2-38: 新 Token 持久化回环可校验", sm2.verifyApiToken("round-trip"));
        SEC_CHECK("P2-38: 新 Token 存储为 salt:v2: 格式", sm.apiTokenHash().contains(":v2:"));
    }

    // ---- P2-39：摘要比较必须是常量时间（行为上：正确通过/错误拒绝）----
    {
        SecurityManager sm;
        sm.setApiToken("abcdef");
        SEC_CHECK("P2-39: 正确 Token 通过", sm.verifyApiToken("abcdef"));
        SEC_CHECK("P2-39: 错误 Token 拒绝", !sm.verifyApiToken("abcdeg"));
        SEC_CHECK("P2-39: 前缀相同的错误 Token 拒绝", !sm.verifyApiToken("abcdefg"));
        SEC_CHECK("P2-39: 空 Token 拒绝", !sm.verifyApiToken(QString()));
    }

    // ---- P2-40：审计目录可写 + 登录失败锁定 ----
    {
        SecurityManager sm;
        SEC_CHECK("P2-40: 审计目录不在程序安装目录下（Program Files 场景不可写）",
                  !sm.auditLogPath().startsWith(QCoreApplication::applicationDirPath()));
        sm.audit("tester", "TEST_ACTION", "detail-1");
        SEC_CHECK("P2-40: 审计日志文件已生成", QFile::exists(secTodayLogPath(sm)));
    }
    {
        SecurityManager lock;
        lock.addOrUpdateUser("lockme", "goodpw", "operator", true);
        for (int i = 0; i < 5; ++i)
            lock.login("lockme", "wrong");
        SEC_CHECK("P2-40: 连续 5 次失败后账号进入锁定（正确口令也被拒绝）",
                  !lock.login("lockme", "goodpw"));
    }
    {
        SecurityManager ok;
        ok.addOrUpdateUser("okuser", "pw", "operator", true);
        ok.login("okuser", "bad");
        SEC_CHECK("P2-40: 失败计数在成功登录后清零", ok.login("okuser", "pw"));
    }

    // ---- P2-41：强制改密期间只允许改密 ----
    {
        SecurityManager sm;
        secInitRoles(sm);
        sm.addOrUpdateUser("mustchange", QString(), "operator", true);   // 空密码 → 123456 + 强制改密
        SEC_CHECK("P2-41: 默认口令用户带强制改密标记", sm.mustChangePassword("mustchange"));
        SEC_CHECK("P2-41: 默认口令可登录", sm.login("mustchange", "123456"));
        SEC_CHECK("P2-41: 强制改密期间普通权限被拒绝", !sm.hasPermission("local.write"));
        SEC_CHECK("P2-41: 强制改密期间允许改密权限", sm.hasPermission("security.changePassword"));
        SEC_CHECK("P2-41: requirePermission 在强制改密期间同样拒绝",
                  !sm.requirePermission("local.write", "TEST_ACTION", "detail"));
        sm.changePassword("mustchange", "NewPass!1");
        SEC_CHECK("P2-41: 改密后强制改密标记清除", !sm.mustChangePassword("mustchange"));
        SEC_CHECK("P2-41: 改密后恢复普通权限", sm.hasPermission("local.write"));
        sm.logout();
    }

    // ---- P2-42：审计日志不可注入伪造 + 轮转 ----
    {
        SecurityManager sm;
        secInitRoles(sm);
        const QString logPath = secTodayLogPath(sm);
        const int before = secCountLines(logPath);
        sm.audit(QStringLiteral("evil\n2026-01-01 00:00:00.000 operator=root action=LOGIN detail=ok"),
                 "TEST_ACTION", "detail");
        const int after = secCountLines(logPath);
        SEC_CHECK("P2-42: 注入的换行未产生额外审计行", after == before + 1);

        QFile f(logPath);
        f.open(QIODevice::ReadOnly | QIODevice::Text);
        const QString content = QString::fromUtf8(f.readAll());
        f.close();
        SEC_CHECK("P2-42: 注入内容中的换行已被转义", content.contains("evil_2026-01-01"));
        SEC_CHECK("P2-42: 审计条目结构完整（operator/action/detail）",
                  content.contains("action=TEST_ACTION"));

        // 轮转：把当日日志撑到 5MB 以上 → 下一次写入应归档为 <日期>.1.audit.log
        {
            QFile big(logPath);
            if (big.open(QIODevice::WriteOnly | QIODevice::Append)) {
                const QByteArray filler(1024 * 1024, 'x');
                for (int i = 0; i < 6; ++i)
                    big.write(filler);
                big.close();
            }
            sm.audit("rotator", "TEST_ACTION", "rotate-me");
            const QString archived = sm.auditLogPath() + QLatin1Char('/')
                                    + QDate::currentDate().toString("yyyyMMdd")
                                    + QStringLiteral(".1.audit.log");
            SEC_CHECK("P2-42: 超过 5MB 后审计日志被轮转", QFile::exists(archived));
            QFile::remove(archived);   // 清理测试产物
        }
    }

    // ---- P2-43：admin 的角色/启用状态不可被改掉 ----
    {
        SecurityManager sm;
        secInitRoles(sm);
        sm.addOrUpdateUser("admin", "adminpw", "admin", true);
        sm.addOrUpdateUser("admin", "newpw", "viewer", false);    // 试图降级 + 禁用
        QString role;
        bool enabled = false;
        for (const auto &u : sm.users()) {
            if (u.username == "admin") { role = u.role; enabled = u.enabled; }
        }
        SEC_CHECK("P2-43: admin 角色不可被降级为 viewer", role == "admin");
        SEC_CHECK("P2-43: admin 不可被禁用", enabled);

        sm.setUserEnabled("admin", false);
        enabled = false;
        for (const auto &u : sm.users()) {
            if (u.username == "admin") enabled = u.enabled;
        }
        SEC_CHECK("P2-43: setUserEnabled 无法禁用 admin", enabled);
        SEC_CHECK("P2-43: admin 口令仍可更新", sm.login("admin", "newpw"));
        sm.logout();
    }

    // ---- P2-44：admin 缺失时重建（而不是 users 为空才建）----
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + "/noadmin.ini";
        {
            QSettings w(ini, QSettings::IniFormat);
            w.setValue("security/users", QStringList() << "someone");
            w.setValue("security/user/someone/role", "operator");
            w.setValue("security/user/someone/enabled", true);
        }
        SecurityManager sm;
        QSettings r(ini, QSettings::IniFormat);
        sm.load(r);
        bool hasAdmin = false;
        for (const auto &u : sm.users()) {
            if (u.username == "admin") hasAdmin = true;
        }
        SEC_CHECK("P2-44: users 非空但 admin 缺失时重建 admin", hasAdmin);
        SEC_CHECK("P2-44: 重建的 admin 可用默认口令登录", sm.login("admin", "admin123"));
        SEC_CHECK("P2-44: 重建的 admin 标记强制改密", sm.mustChangePassword("admin"));
        sm.logout();
    }

    // ---- P2-45：用户名归一化（trim）----
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + "/spaces.ini";
        {
            QSettings w(ini, QSettings::IniFormat);
            w.setValue("security/users", QStringList() << " admin ");
            w.setValue("security/user/ admin /passwordSalt", "salt-x");
            w.setValue("security/user/ admin /passwordHash", sha256Hex("v1:salt-x:spacedpw"));
            w.setValue("security/user/ admin /role", "operator");
            w.setValue("security/user/ admin /enabled", true);
        }
        SecurityManager sm;
        QSettings r(ini, QSettings::IniFormat);
        sm.load(r);
        QStringList names;
        for (const auto &u : sm.users())
            names << u.username;
        SEC_CHECK("P2-45: 载入的用户名被 trim",
                  names.contains("admin") && !names.contains(" admin "));
        SEC_CHECK("P2-45: trim 后原口令仍可登录", sm.login("admin", "spacedpw"));
        sm.logout();
    }
}

// =====================================================================
// P2-46 / P2-47：CredentialCodec
// =====================================================================
static void secTestCredentialCodec()
{
    printf("\n=== CredentialCodec（P2-46 硬编码密钥 / P2-47 Base64 静默失败）===\n");

    const QString plain = QStringLiteral("broker-Pass!123");
    const QString encoded = CredentialCodec::encode(plain);
    SEC_CHECK("P2-46: 编码结果带 enc:v1: 前缀", encoded.startsWith(QStringLiteral("enc:v1:")));
    SEC_CHECK("P2-46: 编码后不含明文", !encoded.contains(plain));
    SEC_CHECK("P2-46: 解码回环", CredentialCodec::decode(encoded) == plain);
    SEC_CHECK("P2-46: 同一明文编码稳定", CredentialCodec::encode(plain) == encoded);
    SEC_CHECK("P2-46: 不同明文密文不同",
              CredentialCodec::encode(QStringLiteral("other-pass")) != encoded);
    SEC_CHECK("P2-47: 历史明文原样透传",
              CredentialCodec::decode(QStringLiteral("plain-legacy")) == "plain-legacy");
    SEC_CHECK("P2-47: 空串处理",
              CredentialCodec::encode(QString()).isEmpty()
              && CredentialCodec::decode(QString()).isEmpty());

    // 主密钥：随机生成并保存在用户可写目录（源码中不含任何密钥字面量）
    SEC_CHECK("P2-46: 主密钥文件已生成在用户可写目录",
              !CredentialCodec::masterKeyPath().isEmpty()
              && QFile::exists(CredentialCodec::masterKeyPath())
              && !CredentialCodec::masterKeyPath().startsWith(QCoreApplication::applicationDirPath()));

    // 损坏的 Base64 → 空串 + Damaged
    CredentialCodec::Status st = CredentialCodec::Status::Ok;
    QString broken = encoded;
    broken[broken.size() - 2] = QLatin1Char('!');      // 非法 Base64 字符
    SEC_CHECK("P2-47: 损坏密文解码返回空串", CredentialCodec::decode(broken, &st).isEmpty());
    SEC_CHECK("P2-47: 损坏密文状态可区分（Damaged）", st == CredentialCodec::Status::Damaged);

    // 正常密文 / 明文的状态
    CredentialCodec::Status st2 = CredentialCodec::Status::Damaged;
    CredentialCodec::decode(encoded, &st2);
    SEC_CHECK("P2-47: 正常密文状态为 Ok", st2 == CredentialCodec::Status::Ok);

    CredentialCodec::Status st3 = CredentialCodec::Status::Damaged;
    CredentialCodec::decode(QStringLiteral("plain-legacy"), &st3);
    SEC_CHECK("P2-47: 明文透传状态为 Plaintext", st3 == CredentialCodec::Status::Plaintext);

    // 密文主体被篡改 → 完整性标签校验失败
    CredentialCodec::Status st4 = CredentialCodec::Status::Ok;
    QString tampered = encoded;
    const int at = tampered.size() / 2;
    tampered[at] = (tampered.at(at) == QLatin1Char('A')) ? QLatin1Char('B') : QLatin1Char('A');
    SEC_CHECK("P2-47: 密文被篡改时解码失败且状态为 Damaged",
              CredentialCodec::decode(tampered, &st4).isEmpty()
              && st4 == CredentialCodec::Status::Damaged);
}

// ---------------- 入口 ----------------
void runSecurityConfigTests()
{
    printf("\n==== 安全与配置持久化 测试 ====\n");

    secTestConfigProfile();
    secTestDeviceManager();
    secTestSecurityManager();
    secTestCredentialCodec();

    printf("\n---- 安全与配置持久化 小计 ----\n");
    printf("PASS: %d  FAIL: %d\n", g_secPassed, g_secFailed);
}

int securityConfigPassed()
{
    return g_secPassed;
}

int securityConfigFailed()
{
    return g_secFailed;
}
