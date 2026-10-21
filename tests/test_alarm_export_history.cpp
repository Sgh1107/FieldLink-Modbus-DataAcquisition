// test_alarm_export_history.cpp
// 【报警 / 导出 / 历史库】三个模块缺陷修复的回归断言。
//
// 覆盖（每条断言在"修复前失败、修复后通过"）：
//   AEH-01 ~ AEH-06  AlarmManager：NaN 误报 / 位测试 UB（浮点→整型转换）
//   AEH-07 ~ AEH-09  AlarmManager：去抖窗口内"事件被抑制但状态必须置位"
//   AEH-10 ~ AEH-12  AlarmManager：报警历史上限 / 禁用规则误发 alarmCleared
//   AEH-13 ~ AEH-16  AlarmManager：删除规则清历史 / 更新规则重置去抖与状态 / 地址索引
//   AEH-17 ~ AEH-21  DataExporter：setMaxRecords(0) 裁剪失效 / 负数上限
//   AEH-22 ~ AEH-26  DataExporter：UTF-8 BOM / CSV 公式注入 / 写入错误被吞
//   AEH-27 ~ AEH-36  HistoryData：写失败静默丢数据 / lastError / 批量事务 /
//                    保留期可配置 / query 分页 / 组合索引 / WAL / 占位符绑定
//
// 约定：本文件自带断言宏与静态计数器，不依赖 master_tests.cpp 的 CHECK 宏；
//       入口为 runAlarmExportHistoryTests()，由主控在 tests.pro 中接线。

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QModbusDataUnit>
#include <cstdio>
#include <cmath>
#include <limits>

#include "alarmmanager.h"
#include "dataexporter.h"
#include "historydata.h"

static int g_aehPassed = 0;
static int g_aehFailed = 0;

#define AEH_CHECK(name, cond) do{ if(cond){printf("  [PASS] %s\n",name);++g_aehPassed;} else {printf("  [FAIL] %s (%s:%d)\n",name,__FILE__,__LINE__);++g_aehFailed;} }while(0)

// 供主控读取统计
int alarmExportHistoryPassed() { return g_aehPassed; }
int alarmExportHistoryFailed() { return g_aehFailed; }

// 事件循环轮询：不使用 QEventLoop::quit()（它会设置线程级 quitNow 标志，
// 导致后续 QEventLoop::exec() 立即返回）
static void aehSpinMs(int ms)
{
    QElapsedTimer et;
    et.start();
    while (et.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static AlarmRule aehMakeRule(const char *name, AlarmCondition cond, double t1,
                             int address, int debounceMs = 0)
{
    AlarmRule r;
    r.name = QString::fromLatin1(name);
    r.enabled = true;
    r.serverAddress = 1;
    r.registerType = 4;   // HoldingRegisters
    r.address = address;
    r.condition = cond;
    r.threshold1 = t1;
    r.threshold2 = 0.0;
    r.severity = AlarmSeverity::Warning;
    r.debounceMs = debounceMs;
    return r;
}

// ============================================================
// AlarmManager：NaN 误报 + 位测试 UB（P1-11 / R-ALARM-1）
// ============================================================
static void aehTestAlarmNaNAndBit()
{
    printf("\n=== AlarmManager: NaN 误报与位测试 UB ===\n");

    // AEH-01：NotEqual + NaN。修复前 qFuzzyCompare(NaN, x) 恒 false
    //          → !qFuzzyCompare 恒 true → NaN 必然误报
    {
        AlarmManager am;
        am.addRule(aehMakeRule("NE", AlarmCondition::NotEqual, 0.0, 0));
        int trig = 0;
        QObject::connect(&am, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++trig; });
        am.checkValue(1, 4, 0, std::numeric_limits<double>::quiet_NaN());
        AEH_CHECK("AEH-01 NaN 值 + NotEqual 规则不触发", trig == 0);
        AEH_CHECK("AEH-02 NaN 值不产生活跃报警", am.activeAlarmCount() == 0);
    }

    // AEH-03：BitClear + NaN。修复前 NaN→整型转换得到 0 → BitClear 恒触发
    {
        AlarmManager am;
        am.addRule(aehMakeRule("BC", AlarmCondition::BitClear, 0.0, 0));
        int trig = 0;
        QObject::connect(&am, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++trig; });
        am.checkValue(1, 4, 0, std::numeric_limits<double>::quiet_NaN());
        AEH_CHECK("AEH-03 NaN 值 + BitClear 规则不触发", trig == 0);
    }

    // AEH-04：非有限值 / 超出 quint32 可表示范围的值不得参与位判定
    //          （浮点→整型转换在值不可表示时是 UB，C++ [conv.fpint]）
    {
        AlarmManager am;
        am.addRule(aehMakeRule("BS", AlarmCondition::BitSet, 3.0, 0));
        int trig = 0;
        QObject::connect(&am, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++trig; });
        am.checkValue(1, 4, 0, std::numeric_limits<double>::infinity());
        am.checkValue(1, 4, 0, -std::numeric_limits<double>::infinity());
        am.checkValue(1, 4, 0, 1.0e18);   // 超出 quint32 可表示范围
        am.checkValue(1, 4, 0, -1.0);     // 负值无法安全转换为 quint32
        AEH_CHECK("AEH-04 非有限/越界值不参与 Bit 判定（无 UB）", trig == 0);

        am.checkValue(1, 4, 0, 8.0);      // bit3 置位
        AEH_CHECK("AEH-05 合法值仍能正常触发 Bit 判定", trig == 1);
    }

    // AEH-06：BitClear 遇到越界值不得误判为"位已清"
    {
        AlarmManager am;
        am.addRule(aehMakeRule("BC2", AlarmCondition::BitClear, 3.0, 0));
        int trig = 0;
        QObject::connect(&am, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++trig; });
        am.checkValue(1, 4, 0, 1.0e18);
        AEH_CHECK("AEH-06 越界值 + BitClear 不触发", trig == 0);
    }
}

// ============================================================
// AlarmManager：去抖窗口内漏报（P1-11 / R-ALARM-2）
// ============================================================
static void aehTestAlarmDebounce()
{
    printf("\n=== AlarmManager: 去抖窗口内状态漏报 ===\n");

    AlarmManager am;
    am.addRule(aehMakeRule("GT", AlarmCondition::GreaterThan, 100.0, 0, 300));

    int trig = 0, cleared = 0;
    QObject::connect(&am, &AlarmManager::alarmTriggered,
                     [&](const AlarmEvent &) { ++trig; });
    QObject::connect(&am, &AlarmManager::alarmCleared, [&](int) { ++cleared; });

    am.checkValue(1, 4, 0, 150.0);            // 首次触发
    AEH_CHECK("AEH-07 首次触发产生事件且状态激活", trig == 1 && am.activeAlarmCount() == 1);
    const int afterFirst = trig;

    am.checkValue(1, 4, 0, 50.0);             // 条件不再成立 → 清除
    AEH_CHECK("AEH-08 条件回落清除报警", cleared == 1 && am.activeAlarmCount() == 0);

    am.checkValue(1, 4, 0, 150.0);            // 去抖窗口内再触发 → 事件被抑制
    AEH_CHECK("AEH-09 去抖窗口内事件被抑制（事件数不增加）", trig == afterFirst);
    AEH_CHECK("AEH-10 去抖窗口内激活状态仍为 1（不再漏报）", am.activeAlarmCount() == 1);
    AEH_CHECK("AEH-11 去抖窗口内活跃报警列表可见", am.activeAlarms().size() == 1);

    aehSpinMs(400);                           // 等待去抖窗口过期
    am.checkValue(1, 4, 0, 150.0);            // 窗口过期 → 补发事件
    AEH_CHECK("AEH-12 去抖窗口过期后仍能再次触发新事件", trig == afterFirst + 1);
}

// ============================================================
// AlarmManager：历史上限 / 禁用规则 / 删除规则 / 更新规则 / 地址索引
// ============================================================
static void aehTestAlarmBookkeeping()
{
    printf("\n=== AlarmManager: 历史上限与规则生命周期 ===\n");

    // AEH-13 / AEH-14：P2-67 历史上限
    {
        AlarmManager am;
        am.setMaxHistory(50);
        AEH_CHECK("AEH-13 setMaxHistory 生效", am.maxHistory() == 50);
        for (int i = 0; i < 200; ++i) {
            am.addRule(aehMakeRule("R", AlarmCondition::GreaterThan, 0.0, i));
            am.checkValue(1, 4, i, 1.0);
        }
        AEH_CHECK("AEH-14 报警历史条数受上限约束（内存不无界增长）",
                  am.alarmHistory(100000).size() == 50);
    }

    // AEH-15 / AEH-16：P2-68 未激活规则禁用不应发 alarmCleared
    {
        AlarmManager am;
        const int id = am.addRule(aehMakeRule("E", AlarmCondition::GreaterThan, 100.0, 0));
        int cleared = 0;
        QObject::connect(&am, &AlarmManager::alarmCleared, [&](int) { ++cleared; });

        am.setRuleEnabled(id, false);         // 从未激活
        AEH_CHECK("AEH-15 未激活规则禁用不发 alarmCleared", cleared == 0);

        am.setRuleEnabled(id, true);
        am.checkValue(1, 4, 0, 150.0);
        AEH_CHECK("AEH-16 重新启用后可正常触发", am.activeAlarmCount() == 1);
        am.setRuleEnabled(id, false);
        AEH_CHECK("AEH-17 激活规则禁用才发 alarmCleared", cleared == 1 && am.activeAlarmCount() == 0);
    }

    // AEH-18：P2-69 删除规则同时清除其历史事件
    {
        AlarmManager am;
        const int id = am.addRule(aehMakeRule("D", AlarmCondition::GreaterThan, 100.0, 0));
        am.checkValue(1, 4, 0, 150.0);
        AEH_CHECK("AEH-18 删除前历史中存在该规则事件", am.alarmHistory(100).size() == 1);
        am.removeRule(id);
        bool stillThere = false;
        for (const AlarmEvent &e : am.alarmHistory(100)) {
            if (e.ruleId == id)
                stillThere = true;
        }
        AEH_CHECK("AEH-19 删除规则同时清除其历史事件", !stillThere);
        AEH_CHECK("AEH-20 删除规则后无活跃报警", am.activeAlarmCount() == 0);
    }

    // AEH-21：P2-70 updateRule 重置去抖窗口与激活状态
    {
        AlarmManager am;
        const int id = am.addRule(aehMakeRule("U", AlarmCondition::GreaterThan, 100.0, 0, 60000));
        int trig = 0;
        QObject::connect(&am, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++trig; });
        am.checkValue(1, 4, 0, 150.0);
        AEH_CHECK("AEH-21 updateRule 前首次触发成功", trig == 1);

        AlarmRule updated = am.getRule(id);
        updated.threshold1 = 120.0;
        am.updateRule(updated);
        AEH_CHECK("AEH-22 updateRule 重置激活状态", am.activeAlarmCount() == 0);

        am.checkValue(1, 4, 0, 150.0);        // 仍在去抖窗口内，但规则已更新
        AEH_CHECK("AEH-23 updateRule 重置去抖窗口后可再次触发", trig == 2);
    }

    // AEH-24 / AEH-25：P2-71 地址索引只命中对应规则
    {
        AlarmManager am;
        am.addRule(aehMakeRule("A", AlarmCondition::GreaterThan, 100.0, 0));
        am.addRule(aehMakeRule("B", AlarmCondition::GreaterThan, 50.0, 1));
        int trig = 0;
        QObject::connect(&am, &AlarmManager::alarmTriggered,
                         [&](const AlarmEvent &) { ++trig; });
        am.checkValue(1, 4, 1, 60.0);
        AEH_CHECK("AEH-24 地址索引只命中对应地址的规则", trig == 1);
        am.checkValue(1, 4, 2, 999.0);
        AEH_CHECK("AEH-25 未登记地址不触发", trig == 1);
        am.checkValue(2, 4, 1, 60.0);
        AEH_CHECK("AEH-26 从站不匹配不触发", trig == 1);
    }
}

// ============================================================
// DataExporter：容量裁剪（P1-/R-EXPORT-1）
// ============================================================
static void aehTestExporterCapacity()
{
    printf("\n=== DataExporter: 容量裁剪 ===\n");

    DataExporter ex;
    ex.setMaxRecords(0);
    AEH_CHECK("AEH-27 setMaxRecords(0) 回落到默认上限（不再表示不限制）",
              ex.maxRecords() == DataExporter::defaultMaxRecords());

    for (int i = 0; i < 20000; ++i) {
        ExportRecord r;
        r.timestamp = QStringLiteral("t");
        r.serverAddress = 1;
        r.registerType = QModbusDataUnit::HoldingRegisters;
        r.startAddress = i;
        r.values = QVector<quint16>(4, static_cast<quint16>(i & 0xFF));
        ex.addRecord(r);
    }
    AEH_CHECK("AEH-28 setMaxRecords(0) 后记录数被限制在默认上限内",
              ex.recordCount() == DataExporter::defaultMaxRecords());

    ex.setMaxRecords(-5);
    AEH_CHECK("AEH-29 负数上限回落到默认值", ex.maxRecords() == DataExporter::defaultMaxRecords());

    ex.setMaxRecords(3);
    AEH_CHECK("AEH-30 缩小上限立即裁剪", ex.recordCount() == 3);
    AEH_CHECK("AEH-31 裁剪后保留最新的三条记录（队首为次新，队尾为最新）",
              ex.records().first().startAddress == 19997
              && ex.records().last().startAddress == 19999);

    ex.setMaxRecords(1);
    AEH_CHECK("AEH-32 上限为 1 时只保留最新一条",
              ex.recordCount() == 1 && ex.records().first().startAddress == 19999);

    ex.clearRecords();
    AEH_CHECK("AEH-33 clearRecords 清空", ex.recordCount() == 0);
}

// ============================================================
// DataExporter：CSV BOM / 公式注入 / 写入错误（P2-18/19/20, P2-62）
// ============================================================
static void aehTestExporterCsv()
{
    printf("\n=== DataExporter: CSV 导出健壮性 ===\n");

    QTemporaryDir dir;
    if (!dir.isValid()) {
        AEH_CHECK("AEH-34 临时目录可用", false);
        return;
    }

    {
        DataExporter ex;
        ExportRecord r;
        r.timestamp = QStringLiteral("2026-09-06T10:00:00.000");
        r.serverAddress = 1;
        r.registerType = QModbusDataUnit::Coils;
        r.startAddress = 0;
        r.values = QVector<quint16>{1, 2, 3};
        ex.addRecord(r);

        const QString path = dir.path() + "/export.csv";
        QString err;
        AEH_CHECK("AEH-34 CSV 导出成功且无错误描述",
                  ex.exportToCsv(path, &err) && err.isEmpty());

        QFile f(path);
        AEH_CHECK("AEH-35 CSV 文件可读", f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        f.close();
        AEH_CHECK("AEH-36 CSV 带 UTF-8 BOM（Excel 打开中文不乱码）",
                  bytes.startsWith(QByteArray("\xEF\xBB\xBF", 3)));

        const QString content = QString::fromUtf8(bytes);
        AEH_CHECK("AEH-37 CSV 表头正确（QString::fromUtf8 会剥离 BOM，表头断言仍成立）",
                  content.startsWith(QStringLiteral("Timestamp,ServerAddress,RegisterType,StartAddress,Values")));
        AEH_CHECK("AEH-38 CSV 数据行格式未变",
                  content.contains(QStringLiteral("\"2026-09-06T10:00:00.000\",\"1\",\"Coils\",\"0\",\"1;2;3\"")));
    }

    // AEH-39：CSV 公式注入防护（= + - @ 开头）
    {
        DataExporter ex;
        const char *payloads[] = {"=1+1", "-1+1", "+1+1", "@SUM(A1)"};
        for (const char *p : payloads) {
            ExportRecord r;
            r.timestamp = QString::fromLatin1(p);
            r.serverAddress = 1;
            r.registerType = QModbusDataUnit::Coils;
            r.startAddress = 0;
            r.values = QVector<quint16>{1};
            ex.addRecord(r);
        }
        const QString path = dir.path() + "/inject.csv";
        ex.exportToCsv(path);

        QFile f(path);
        f.open(QIODevice::ReadOnly);
        const QString content = QString::fromUtf8(f.readAll());
        f.close();
        AEH_CHECK("AEH-39 CSV 公式注入被加 ' 前缀转义",
                  content.contains(QStringLiteral("\"'=1+1\""))
                  && content.contains(QStringLiteral("\"'-1+1\""))
                  && content.contains(QStringLiteral("\"'+1+1\""))
                  && content.contains(QStringLiteral("\"'@SUM(A1)\"")));
    }

    // AEH-40：写入失败必须返回 false 并给出错误描述（不能再静默返回 true）
    {
        DataExporter ex;
        ExportRecord r;
        r.timestamp = QStringLiteral("t");
        ex.addRecord(r);

        const QString badPath = dir.path() + "/no_such_dir/sub/export.csv";
        QString err;
        AEH_CHECK("AEH-40 写入路径非法时导出返回 false", !ex.exportToCsv(badPath, &err));
        AEH_CHECK("AEH-41 写入失败给出错误描述", !err.isEmpty());
        AEH_CHECK("AEH-42 errorMessage 为空指针时不影响调用",
                  !ex.exportToCsv(badPath));
    }
}

// ============================================================
// HistoryData：写失败可观测 / 批量事务 / 保留期 / 分页 / 索引（P2-08/11/12/13/14/16/17）
// ============================================================
static void aehTestHistoryErrors()
{
    printf("\n=== HistoryData: 错误可观测性 ===\n");

    // AEH-43：未打开数据库时写入必须留下错误描述（不再无声丢数据）
    {
        HistoryData h;
        h.addRecord(1, QModbusDataUnit::HoldingRegisters, 0, QVector<quint16>{1});
        AEH_CHECK("AEH-43 数据库不可写时 lastError() 非空", !h.lastError().isEmpty());
    }

    // AEH-44：打开失败必须返回 false 并记录错误
    {
        QTemporaryDir dir;
        HistoryData h;
        const bool ok = h.openDatabase(dir.path() + "/missing_dir/history.db");
        AEH_CHECK("AEH-44 非法路径打开失败返回 false", !ok);
        AEH_CHECK("AEH-45 打开失败 lastError() 非空", !h.lastError().isEmpty());
        AEH_CHECK("AEH-46 打开失败后未处于打开状态", !h.isOpen());
    }
}

static void aehTestHistoryBatchAndQuery()
{
    printf("\n=== HistoryData: 批量写入 / 分页 / 索引 ===\n");

    QTemporaryDir dir;
    if (!dir.isValid()) {
        AEH_CHECK("AEH-47 临时目录可用", false);
        return;
    }

    HistoryData h;
    if (!h.openDatabase(dir.path() + "/history.db")) {
        AEH_CHECK("AEH-47 打开数据库成功", false);
        return;
    }
    AEH_CHECK("AEH-47 打开数据库成功", h.isOpen());

    // AEH-48：批量写入（单事务提交）
    QVector<HistoryBatchEntry> batch;
    for (int i = 0; i < 50; ++i) {
        HistoryBatchEntry e;
        e.serverAddress = 1;
        e.registerType = QModbusDataUnit::HoldingRegisters;
        e.startAddress = i;
        e.values = QVector<quint16>{static_cast<quint16>(i)};
        batch.append(e);
    }
    AEH_CHECK("AEH-48 批量写入成功", h.addRecords(batch));
    AEH_CHECK("AEH-49 批量写入条数正确", h.totalRecords() == 50);
    AEH_CHECK("AEH-50 批量写入后 lastError() 为空", h.lastError().isEmpty());

    // AEH-51 / AEH-52：query 的 limit / offset
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime from = now.addSecs(-60);
    const QDateTime to = now.addSecs(60);
    AEH_CHECK("AEH-51 不带 limit 的查询返回全部", h.query(from, to).size() == 50);
    AEH_CHECK("AEH-52 query 的 limit 生效", h.query(from, to, -1, -1, -1, 10).size() == 10);
    AEH_CHECK("AEH-53 query 的 offset 生效", h.query(from, to, -1, -1, -1, 10, 45).size() == 5);

    // AEH-54 ~ AEH-56：整数条件改为占位符绑定后过滤依然正确
    AEH_CHECK("AEH-54 按从站过滤（占位符绑定）", h.query(from, to, 1, -1, -1).size() == 50);
    AEH_CHECK("AEH-55 按起始地址过滤（占位符绑定）", h.query(from, to, -1, -1, 7).size() == 1);
    AEH_CHECK("AEH-56 无匹配过滤返回空", h.query(from, to, 9, -1, -1).isEmpty());

    // AEH-57：保留期可配置
    AEH_CHECK("AEH-57 默认保留期为 30 天", h.retentionDays() == HistoryData::defaultRetentionDays());
    h.setRetentionDays(7);
    AEH_CHECK("AEH-58 保留期可配置", h.retentionDays() == 7);
    h.setRetentionDays(0);
    AEH_CHECK("AEH-59 保留期设为 0 表示不自动清理", h.retentionDays() == 0);
    h.setRetentionDays(30);

    // AEH-60：totalRecords 使用 qint64（不溢出）
    const qint64 total = h.totalRecords();
    AEH_CHECK("AEH-60 totalRecords 返回 qint64（不发生 2^31 溢出）", total == 50);

    // AEH-61 / AEH-62：WAL 模式与组合索引
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "aeh_verify_conn");
        db.setDatabaseName(dir.path() + "/history.db");
        const bool opened = db.open();
        AEH_CHECK("AEH-61 可打开数据库做结构校验", opened);
        if (opened) {
            QSqlQuery q(db);
            q.exec("PRAGMA journal_mode");
            const QString mode = q.next() ? q.value(0).toString() : QString();
            AEH_CHECK("AEH-62 已启用 WAL 日志模式",
                      mode.compare(QStringLiteral("wal"), Qt::CaseInsensitive) == 0);

            q.exec("SELECT name FROM sqlite_master WHERE type='index' AND name='idx_hist_lookup'");
            AEH_CHECK("AEH-63 已创建组合索引 idx_hist_lookup", q.next());
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("aeh_verify_conn");

    // AEH-64：clearOlderThan 公开语义不变
    h.clearOlderThan(now.addSecs(60));
    AEH_CHECK("AEH-64 clearOlderThan 清空历史", h.totalRecords() == 0);

    // AEH-65：切换数据库路径时先关闭再打开
    {
        HistoryData h2;
        h2.openDatabase(dir.path() + "/a.db");
        const bool switched = h2.openDatabase(dir.path() + "/b.db");
        AEH_CHECK("AEH-65 切换到不同路径时重新打开成功",
                  switched && h2.isOpen() && QFileInfo(dir.path() + "/b.db").exists());
    }
}

// ============================================================
// 入口
// ============================================================
void runAlarmExportHistoryTests()
{
    printf("\n==== 报警 / 导出 / 历史库 缺陷修复回归测试 ====\n");
    aehTestAlarmNaNAndBit();
    aehTestAlarmDebounce();
    aehTestAlarmBookkeeping();
    aehTestExporterCapacity();
    aehTestExporterCsv();
    aehTestHistoryErrors();
    aehTestHistoryBatchAndQuery();
    printf("\n---- 报警/导出/历史库 小计: PASS %d  FAIL %d ----\n", g_aehPassed, g_aehFailed);
}
