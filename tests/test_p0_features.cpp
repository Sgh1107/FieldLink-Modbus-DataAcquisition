// test_p0_features.cpp
// P0 功能补齐的回归断言（设备维度点位 / 工程值历史 / 点位报警 / 质量降级）。
//
// 覆盖：
//   P0-01 ~ P0-06  多设备点位隔离（updateFromRaw 按 deviceId 分发）
//   P0-07 ~ P0-10  质量降级：markPointsBad / markRangeBad / markDeviceBad / markStale
//   P0-11 ~ P0-17  工程值归档：写入 / 设备+点位过滤 / 计数 / 时间桶聚合 / 清理
//   P0-18 ~ P0-22  点位报警：按工程值阈值判定 / 设备+点位隔离 / 质量码抑制 / 恢复
//   P0-23 ~ P0-25  新点位字段（deviceId / pollIntervalSec / archiveDeadband）JSON 持久化
//
// 约定：自带断言宏与计数器，入口 runP0FeatureTests()，由 master_tests.cpp 调用。

#include <QCoreApplication>
#include <QDateTime>
#include <QTemporaryDir>
#include <QFile>
#include <QModbusDataUnit>
#include <cstdio>
#include <cmath>

#include "pointmodel.h"
#include "historydata.h"
#include "alarmmanager.h"

static int g_p0Passed = 0;
static int g_p0Failed = 0;

#define P0_CHECK(name, cond) do{ if(cond){printf("  [PASS] %s\n",name);++g_p0Passed;} else {printf("  [FAIL] %s (%s:%d)\n",name,__FILE__,__LINE__);++g_p0Failed;} }while(0)

static PointDefinition p0MakePoint(const QString &name, int deviceId, int address,
                                   int count = 1, int pollSec = 1)
{
    PointDefinition p;
    p.name = name;
    p.deviceId = deviceId;
    p.serverAddress = 1;
    p.registerType = QModbusDataUnit::HoldingRegisters;
    p.address = address;
    p.count = count;
    p.dataType = QStringLiteral("uint16");
    p.pollIntervalSec = pollSec;
    return p;
}

static void testP0DeviceIsolation()
{
    printf("\n=== P0 多设备点位隔离 ===\n");
    PointModel model;
    const int mainPoint = model.addPoint(p0MakePoint(QStringLiteral("主连接"), 0, 10));
    const int devPoint = model.addPoint(p0MakePoint(QStringLiteral("设备1"), 1, 10));

    model.updateFromRaw(1, QModbusDataUnit::HoldingRegisters, 10, QVector<quint16>{111}, 0);
    P0_CHECK("P0-01: 主连接点位收到值", model.value(mainPoint).value == 111.0
             && model.value(mainPoint).quality == DataQuality::Good);
    P0_CHECK("P0-02: 同地址的设备1点位未被串值",
             model.value(devPoint).quality == DataQuality::Unknown);

    model.updateFromRaw(1, QModbusDataUnit::HoldingRegisters, 10, QVector<quint16>{222}, 1);
    P0_CHECK("P0-03: 设备1点位独立更新", model.value(devPoint).value == 222.0);
    P0_CHECK("P0-04: 主连接值保持不变", model.value(mainPoint).value == 111.0);

    // 32 位/多寄存器点位跨区间：只命中与失败区间有交集的点位
    const int wide = model.addPoint(p0MakePoint(QStringLiteral("跨寄存器"), 1, 100, 2));
    model.updateFromRaw(1, QModbusDataUnit::HoldingRegisters, 100, QVector<quint16>{1, 2}, 1);
    P0_CHECK("P0-05: 多寄存器点位正常换算", model.value(wide).quality == DataQuality::Good);
    model.markRangeBad(1, 1, QModbusDataUnit::HoldingRegisters, 100, 1,
                       DataQuality::Bad, QStringLiteral("test"));
    P0_CHECK("P0-06: 失败区间与点位有交集时降级", model.value(wide).quality == DataQuality::Bad);
}

static void testP0QualityDegradation()
{
    printf("\n=== P0 通信质量降级 ===\n");
    PointModel model;
    const int a = model.addPoint(p0MakePoint(QStringLiteral("A"), 0, 1, 1, 1));
    const int b = model.addPoint(p0MakePoint(QStringLiteral("B"), 0, 2, 1, 10));
    const int c = model.addPoint(p0MakePoint(QStringLiteral("C"), 1, 3, 1, 1));
    model.updateFromRaw(1, QModbusDataUnit::HoldingRegisters, 1, QVector<quint16>{1, 2}, 0);
    model.updateFromRaw(1, QModbusDataUnit::HoldingRegisters, 3, QVector<quint16>{3}, 1);

    int qualitySignals = 0;
    QObject::connect(&model, &PointModel::pointQualityChanged,
                     [&](int, DataQuality) { ++qualitySignals; });

    model.markPointsBad({a}, DataQuality::Timeout, QStringLiteral("unit"));
    P0_CHECK("P0-07: 指定点位降级为 Timeout",
             model.value(a).quality == DataQuality::Timeout);
    P0_CHECK("P0-08: 降级会发出 qualityChanged 信号", qualitySignals > 0);

    model.markDeviceBad(1, DataQuality::Bad, QStringLiteral("device disconnected"));
    P0_CHECK("P0-09: 设备断开只降级该设备点位",
             model.value(c).quality == DataQuality::Bad
             && model.value(b).quality == DataQuality::Good);

    // b 的采集周期是 10s → 阈值 max(5000, 30000) = 30s
    const QDateTime now = QDateTime::currentDateTime().addSecs(15);
    const int stale = model.markStale(now, 5000);
    P0_CHECK("P0-10: 陈旧看门狗按 3×采集周期判定",
             stale == 0 && model.value(b).quality == DataQuality::Good);

    const int stale2 = model.markStale(now.addSecs(20), 5000);
    P0_CHECK("P0-11: 超过 3×采集周期后置 Timeout",
             stale2 == 1 && model.value(b).quality == DataQuality::Timeout);
}

static void testP0PointHistory()
{
    printf("\n=== P0 工程值历史 ===\n");
    QTemporaryDir dir;
    P0_CHECK("P0-12: 临时目录可用", dir.isValid());
    HistoryData history;
    P0_CHECK("P0-13: 历史库打开", history.openDatabase(dir.filePath(QStringLiteral("p0.db"))));

    const QDateTime base = QDateTime::fromSecsSinceEpoch(1700000000);   // 固定时间避免跨桶
    QVector<PointHistoryRecord> records;
    auto add = [&](int deviceId, int pointId, int offsetSec, double value, int quality = 0) {
        PointHistoryRecord r;
        r.timestamp = base.addSecs(offsetSec);
        r.deviceId = deviceId;
        r.pointId = pointId;
        r.pointName = QStringLiteral("P%1").arg(pointId);
        r.serverAddress = 1;
        r.value = value;
        r.quality = quality;
        records.append(r);
    };
    add(0, 1, 10, 10.0);
    add(0, 1, 20, 20.0);
    add(0, 1, 30, 30.0);
    add(1, 2, 5, 1000.0, 1);

    P0_CHECK("P0-14: 批量写入工程值成功", history.addPointRecords(records));
    P0_CHECK("P0-15: 工程值总数正确", history.totalPointRecords() == 4);

    const auto all = history.queryPoints(base.addSecs(-10), base.addSecs(60), -1, -1);
    P0_CHECK("P0-16: 不带过滤查询返回全部", all.size() == 4);
    const auto dev0 = history.queryPoints(base.addSecs(-10), base.addSecs(60), 0, -1);
    P0_CHECK("P0-17: 按设备过滤只返回该设备", dev0.size() == 3);
    for (const auto &r : dev0) {
        if (r.deviceId != 0) { P0_CHECK("P0-17: 设备过滤结果含错设备", false); break; }
    }
    const auto point1 = history.queryPoints(base.addSecs(-10), base.addSecs(60), 0, 1, 2, 0);
    P0_CHECK("P0-18: 按点位过滤 + 分页 limit 生效", point1.size() == 2);
    const auto quality = history.queryPoints(base.addSecs(-10), base.addSecs(60), 1, 2);
    P0_CHECK("P0-19: 质量码随工程值归档",
             quality.size() == 1 && quality.first().quality == 1);

    const auto agg = history.aggregatePoints(base.addSecs(-10), base.addSecs(60), 0, 1, 60);
    P0_CHECK("P0-20: 60s 聚合只产生一个时间桶", agg.size() == 1);
    P0_CHECK("P0-21: 聚合 min/max/avg/count 正确",
             !agg.isEmpty() && qFuzzyCompare(agg.first().minValue, 10.0)
             && qFuzzyCompare(agg.first().maxValue, 30.0)
             && qFuzzyCompare(agg.first().avgValue, 20.0)
             && agg.first().count == 3);

    history.clearOlderThan(base.addSecs(15));
    P0_CHECK("P0-22: 清理旧记录同时作用于工程值表", history.totalPointRecords() == 2);
    history.clearAll();
    P0_CHECK("P0-23: 清空同时作用于工程值表", history.totalPointRecords() == 0);
}

static void testP0PointAlarms()
{
    printf("\n=== P0 工程值报警 ===\n");
    AlarmManager alarms;
    int triggered = 0;
    int cleared = 0;
    QObject::connect(&alarms, &AlarmManager::alarmTriggered,
                     [&](const AlarmEvent &) { ++triggered; });
    QObject::connect(&alarms, &AlarmManager::alarmCleared,
                     [&](int) { ++cleared; });

    AlarmRule rule;
    rule.name = QStringLiteral("温度高");
    rule.enabled = true;
    rule.deviceId = 0;
    rule.pointId = 7;
    rule.serverAddress = 1;
    rule.registerType = static_cast<int>(QModbusDataUnit::HoldingRegisters);
    rule.address = 42;
    rule.condition = AlarmCondition::GreaterThan;
    rule.threshold1 = 100.0;
    alarms.addRule(rule);

    alarms.checkPointValue(0, 7, 20.0, 0);
    P0_CHECK("P0-24: 低于工程值阈值不报警", triggered == 0);

    alarms.checkPointValue(0, 7, 150.0, 0);
    P0_CHECK("P0-25: 高于工程值阈值触发报警", triggered == 1);

    // 原始寄存器规则索引不应命中点位规则（同一地址也不会双触发）
    alarms.checkValue(1, static_cast<int>(QModbusDataUnit::HoldingRegisters), 42, 9999.0);
    P0_CHECK("P0-26: 点位规则不参与原始寄存器判定", triggered == 1);

    alarms.checkPointValue(1, 7, 200.0, 0);
    P0_CHECK("P0-27: 设备维度隔离（同点位 id 不同设备不触发）", triggered == 1);

    alarms.checkPointValue(0, 7, 200.0, 1);
    alarms.checkPointValue(0, 7, 200.0, 2);
    alarms.checkPointValue(0, 7, 200.0, 4);
    P0_CHECK("P0-28: Bad/Timeout/Unknown 质量码不参与判定", triggered == 1);

    alarms.checkPointValue(0, 7, 50.0, 0);
    P0_CHECK("P0-29: 回落到阈值以下清除报警", cleared == 1);
}

static void testP0PointPersistence()
{
    printf("\n=== P0 点位新字段持久化 ===\n");
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("points.json"));

    PointModel source;
    PointDefinition point = p0MakePoint(QStringLiteral("持久化"), 3, 77);
    point.pollIntervalSec = 2;
    point.archiveIntervalSec = 15;
    point.archiveDeadband = 0.25;
    const int id = source.addPoint(point);
    P0_CHECK("P0-30: JSON 保存成功", source.saveToFile(path));

    PointModel loaded;
    P0_CHECK("P0-31: JSON 载入成功", loaded.loadFromFile(path));
    const PointDefinition restored = loaded.point(id);
    P0_CHECK("P0-32: deviceId 持久化", restored.deviceId == 3);
    P0_CHECK("P0-33: pollIntervalSec 持久化", restored.pollIntervalSec == 2);
    P0_CHECK("P0-34: archiveDeadband 持久化", qFuzzyCompare(restored.archiveDeadband, 0.25));
}

void runP0FeatureTests()
{
    printf("\n==== P0 功能补齐回归 ====\n");
    testP0DeviceIsolation();
    testP0QualityDegradation();
    testP0PointHistory();
    testP0PointAlarms();
    testP0PointPersistence();
    printf("---- P0 小计 ----\nPASS: %d  FAIL: %d\n", g_p0Passed, g_p0Failed);
}

// 供主控读取统计
int p0FeaturePassed() { return g_p0Passed; }
int p0FeatureFailed() { return g_p0Failed; }
