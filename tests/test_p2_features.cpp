// test_p2_features.cpp
// P2 regression: scheduled reports / database backup, site configuration bundle,
// and point-level write protection / interlock.
//
// Naming: P2W-* write guard, P2P-* point persistence, P2R-* report scheduler,
//         P2S-* site configuration bundle.
// Entry point: runP2FeatureTests(), called from master_tests.cpp.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>
#include <cstdio>

#include "alarmmanager.h"
#include "historydata.h"
#include "pointmodel.h"
#include "reportscheduler.h"
#include "siteconfigbundle.h"
#include "writeguard.h"

static int g_p2Passed = 0;
static int g_p2Failed = 0;

#define P2_CHECK(name, cond) do { \
    if (cond) { printf("  [PASS] %s\n", name); ++g_p2Passed; } \
    else { printf("  [FAIL] %s (%s:%d)\n", name, __FILE__, __LINE__); ++g_p2Failed; } \
} while (0)

int p2FeaturePassed() { return g_p2Passed; }
int p2FeatureFailed() { return g_p2Failed; }

namespace {

PointDefinition makePoint(int id, const QString &name, int deviceId, int address,
                          int count = 1, const QString &dataType = QStringLiteral("uint16"))
{
    PointDefinition point;
    point.id = id;
    point.name = name;
    point.deviceId = deviceId;
    point.serverAddress = 1;
    point.registerType = QModbusDataUnit::HoldingRegisters;
    point.address = address;
    point.count = count;
    point.dataType = dataType;
    return point;
}

PointValue makeValue(int pointId, double value, DataQuality quality = DataQuality::Good)
{
    PointValue v;
    v.pointId = pointId;
    v.value = value;
    v.quality = quality;
    v.timestamp = QDateTime::currentDateTime();
    return v;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

QString findBackupFile(const QString &backupRoot, const QString &suffix)
{
    QDirIterator it(backupRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.endsWith(suffix))
            return path;
    }
    return QString();
}

} // namespace

static void testWriteGuard()
{
    printf("\n=== P2W write guard / interlock ===\n");

    // registerSpan / overlap
    P2_CHECK("P2W-01 default span is 1", WriteGuard::registerSpan(makePoint(1, "a", 0, 0)) == 1);
    P2_CHECK("P2W-02 float32 span is at least 2",
             WriteGuard::registerSpan(makePoint(1, "a", 0, 0, 1, "float32")) == 2);
    P2_CHECK("P2W-03 float64 span is at least 4",
             WriteGuard::registerSpan(makePoint(1, "a", 0, 0, 1, "float64")) == 4);
    P2_CHECK("P2W-04 explicit count wins when larger",
             WriteGuard::registerSpan(makePoint(1, "a", 0, 0, 6, "uint16")) == 6);

    const PointDefinition point = makePoint(10, "Valve", 0, 100, 10);
    P2_CHECK("P2W-05 overlap same range",
             WriteGuard::overlaps(point, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 10));
    P2_CHECK("P2W-06 overlap partial",
             WriteGuard::overlaps(point, 0, 1, QModbusDataUnit::HoldingRegisters, 105, 2));
    P2_CHECK("P2W-07 no overlap just after range",
             !WriteGuard::overlaps(point, 0, 1, QModbusDataUnit::HoldingRegisters, 110, 1));
    P2_CHECK("P2W-08 no overlap just before range",
             !WriteGuard::overlaps(point, 0, 1, QModbusDataUnit::HoldingRegisters, 90, 10));
    P2_CHECK("P2W-09 different device does not overlap",
             !WriteGuard::overlaps(point, 1, 1, QModbusDataUnit::HoldingRegisters, 100, 1));
    P2_CHECK("P2W-10 different slave does not overlap",
             !WriteGuard::overlaps(point, 0, 2, QModbusDataUnit::HoldingRegisters, 100, 1));
    P2_CHECK("P2W-11 different register type does not overlap",
             !WriteGuard::overlaps(point, 0, 1, QModbusDataUnit::Coils, 100, 1));
    P2_CHECK("P2W-12 zero-length write does not overlap",
             !WriteGuard::overlaps(point, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 0));

    // No protection: allowed
    {
        QVector<PointDefinition> points{point};
        const WriteGuardResult result = WriteGuard::check(
            points, {}, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 10);
        P2_CHECK("P2W-13 unprotected point allows write", result.allowed);
        P2_CHECK("P2W-14 affected point ids are reported", result.pointIds.contains(10));
    }

    // Write protection
    {
        PointDefinition guarded = point;
        guarded.writeProtected = true;
        QVector<PointDefinition> points{guarded};
        const WriteGuardResult result = WriteGuard::check(
            points, {}, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1);
        P2_CHECK("P2W-15 write-protected point is rejected", !result.allowed);
        P2_CHECK("P2W-16 rejection flags protection", result.blockedByProtection);
        P2_CHECK("P2W-17 rejection reason mentions point name", result.reason.contains("Valve"));
        P2_CHECK("P2W-18 rejection reason is not empty", !result.reason.isEmpty());
    }

    // Non-overlapping protected point must not block an unrelated write
    {
        PointDefinition guarded = makePoint(11, "Other", 0, 200, 1);
        guarded.writeProtected = true;
        QVector<PointDefinition> points{guarded};
        const WriteGuardResult result = WriteGuard::check(
            points, {}, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1);
        P2_CHECK("P2W-19 unrelated protected point does not block", result.allowed);
        P2_CHECK("P2W-20 unrelated point is not in affected list", result.pointIds.isEmpty());
    }

    // Interlock: missing linked point
    {
        PointDefinition guarded = point;
        guarded.interlockEnabled = true;
        guarded.interlockPointId = 99;
        guarded.interlockMin = 0.0;
        guarded.interlockMax = 10.0;
        QVector<PointDefinition> points{guarded};
        const WriteGuardResult result = WriteGuard::check(
            points, {}, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1);
        P2_CHECK("P2W-21 missing interlock point is rejected", !result.allowed);
        P2_CHECK("P2W-22 missing interlock flags interlock", result.blockedByInterlock);
    }

    // Interlock: no reading
    {
        PointDefinition guarded = point;
        guarded.interlockEnabled = true;
        guarded.interlockPointId = 20;
        guarded.interlockMin = 0.0;
        guarded.interlockMax = 10.0;
        PointDefinition linked = makePoint(20, "Pressure", 0, 300, 1);
        QVector<PointDefinition> points{guarded, linked};
        const WriteGuardResult result = WriteGuard::check(
            points, {}, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1);
        P2_CHECK("P2W-23 interlock without reading is rejected", !result.allowed);
        P2_CHECK("P2W-24 reason mentions interlock point", result.reason.contains("Pressure"));
    }

    // Interlock: bad quality
    {
        PointDefinition guarded = point;
        guarded.interlockEnabled = true;
        guarded.interlockPointId = 20;
        guarded.interlockMin = 0.0;
        guarded.interlockMax = 10.0;
        PointDefinition linked = makePoint(20, "Pressure", 0, 300, 1);
        QVector<PointDefinition> points{guarded, linked};
        QVector<PointValue> values{makeValue(20, 5.0, DataQuality::Timeout)};
        const WriteGuardResult result = WriteGuard::check(
            points, values, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1);
        P2_CHECK("P2W-25 interlock with bad quality is rejected", !result.allowed);
        P2_CHECK("P2W-26 reason mentions quality", result.reason.contains("Timeout"));
    }

    // Interlock: value out of range (both sides)
    {
        PointDefinition guarded = point;
        guarded.interlockEnabled = true;
        guarded.interlockPointId = 20;
        guarded.interlockMin = 2.0;
        guarded.interlockMax = 8.0;
        PointDefinition linked = makePoint(20, "Pressure", 0, 300, 1);
        QVector<PointDefinition> points{guarded, linked};
        QVector<PointValue> low{makeValue(20, 1.0)};
        QVector<PointValue> high{makeValue(20, 9.0)};
        P2_CHECK("P2W-27 interlock below minimum is rejected",
                 !WriteGuard::check(points, low, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1).allowed);
        P2_CHECK("P2W-28 interlock above maximum is rejected",
                 !WriteGuard::check(points, high, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1).allowed);
    }

    // Interlock: in range / boundaries
    {
        PointDefinition guarded = point;
        guarded.interlockEnabled = true;
        guarded.interlockPointId = 20;
        guarded.interlockMin = 2.0;
        guarded.interlockMax = 8.0;
        PointDefinition linked = makePoint(20, "Pressure", 0, 300, 1);
        QVector<PointDefinition> points{guarded, linked};
        P2_CHECK("P2W-29 interlock in range allows",
                 WriteGuard::check(points, {makeValue(20, 5.0)}, 0, 1,
                                   QModbusDataUnit::HoldingRegisters, 100, 1).allowed);
        P2_CHECK("P2W-30 interlock lower boundary allows",
                 WriteGuard::check(points, {makeValue(20, 2.0)}, 0, 1,
                                   QModbusDataUnit::HoldingRegisters, 100, 1).allowed);
        P2_CHECK("P2W-31 interlock upper boundary allows",
                 WriteGuard::check(points, {makeValue(20, 8.0)}, 0, 1,
                                   QModbusDataUnit::HoldingRegisters, 100, 1).allowed);
    }

    // Protection wins over interlock
    {
        PointDefinition guarded = point;
        guarded.writeProtected = true;
        guarded.interlockEnabled = true;
        guarded.interlockPointId = 20;
        PointDefinition linked = makePoint(20, "Pressure", 0, 300, 1);
        QVector<PointDefinition> points{guarded, linked};
        const WriteGuardResult result = WriteGuard::check(
            points, {makeValue(20, 5.0)}, 0, 1, QModbusDataUnit::HoldingRegisters, 100, 1);
        P2_CHECK("P2W-32 protection takes precedence over interlock",
                 !result.allowed && result.blockedByProtection && !result.blockedByInterlock);
    }

    // 32-bit point spans two registers even when count == 1
    {
        PointDefinition guarded = makePoint(30, "Flow", 0, 100, 1, "float32");
        guarded.writeProtected = true;
        QVector<PointDefinition> points{guarded};
        P2_CHECK("P2W-33 32-bit point protects second register",
                 !WriteGuard::check(points, {}, 0, 1,
                                    QModbusDataUnit::HoldingRegisters, 101, 1).allowed);
        P2_CHECK("P2W-34 32-bit point does not protect beyond its span",
                 WriteGuard::check(points, {}, 0, 1,
                                   QModbusDataUnit::HoldingRegisters, 102, 1).allowed);
    }

    P2_CHECK("P2W-35 quality name Good", WriteGuard::qualityName(DataQuality::Good) == "Good");
    P2_CHECK("P2W-36 quality name OutOfRange",
             WriteGuard::qualityName(DataQuality::OutOfRange) == "OutOfRange");
}

static void testPointPersistence()
{
    printf("\n=== P2P point write protection / interlock persistence ===\n");

    QTemporaryDir dir;
    P2_CHECK("P2P-01 temp dir exists", dir.isValid());
    const QString jsonPath = dir.filePath("points.json");
    const QString csvPath = dir.filePath("points.csv");

    PointModel model;
    PointDefinition first = makePoint(0, "Pump", 0, 10);
    PointDefinition second = makePoint(0, "Valve", 0, 20);
    second.writeProtected = true;
    second.writeProtectNote = QStringLiteral("safety");
    second.interlockEnabled = true;
    second.interlockPointId = 1;
    second.interlockMin = 1.5;
    second.interlockMax = 9.5;
    model.addPoint(first);
    model.addPoint(second);

    P2_CHECK("P2P-02 save JSON", model.saveToFile(jsonPath));
    PointModel loaded;
    P2_CHECK("P2P-03 load JSON", loaded.loadFromFile(jsonPath));
    P2_CHECK("P2P-04 two points round-trip", loaded.points().size() == 2);
    if (loaded.points().size() == 2) {
        const PointDefinition &p = loaded.points().at(1);
        P2_CHECK("P2P-05 writeProtected round-trip", p.writeProtected);
        P2_CHECK("P2P-06 note round-trip", p.writeProtectNote == "safety");
        P2_CHECK("P2P-07 interlock enabled round-trip", p.interlockEnabled);
        P2_CHECK("P2P-08 interlock point id round-trip", p.interlockPointId == 1);
        P2_CHECK("P2P-09 interlock minimum round-trip", qFuzzyCompare(p.interlockMin, 1.5));
        P2_CHECK("P2P-10 interlock maximum round-trip", qFuzzyCompare(p.interlockMax, 9.5));
    }

    // Old JSON files without the new keys must keep safe defaults.
    const QByteArray oldJson = R"([{"name":"Old","serverAddress":1,"registerType":3,"address":5,"count":1,"dataType":"uint16","scale":1,"offset":0,"unit":"","alarmLow":0,"alarmHigh":65535,"archiveEnabled":true,"archiveIntervalSec":5,"byteOrder":"ABCD","deviceId":0,"pollIntervalSec":1,"archiveDeadband":0}])";
    const QString oldJsonPath = dir.filePath("old.json");
    P2_CHECK("P2P-11 write old JSON", writeFile(oldJsonPath, oldJson));
    PointModel oldModel;
    P2_CHECK("P2P-12 load old JSON", oldModel.loadFromFile(oldJsonPath));
    P2_CHECK("P2P-13 old JSON defaults to unprotected",
             oldModel.points().size() == 1 && !oldModel.points().first().writeProtected);
    P2_CHECK("P2P-14 old JSON defaults to no interlock",
             oldModel.points().size() == 1 && !oldModel.points().first().interlockEnabled);

    // CSV export contains the new columns and round-trips.
    P2_CHECK("P2P-15 export CSV", model.exportToCsv(csvPath));
    const QString csv = QString::fromUtf8(readFile(csvPath));
    P2_CHECK("P2P-16 CSV header contains writeProtected", csv.contains("writeProtected"));
    P2_CHECK("P2P-17 CSV header contains interlockPointId", csv.contains("interlockPointId"));
    P2_CHECK("P2P-18 CSV contains protection flag", csv.contains(",1,safety,1,"));

    PointModel csvLoaded;
    int imported = 0;
    QString error;
    P2_CHECK("P2P-19 import CSV", csvLoaded.importFromCsv(csvPath, false, &imported, &error));
    P2_CHECK("P2P-20 CSV imported both points", imported == 2);
    if (csvLoaded.points().size() == 2) {
        const PointDefinition &p = csvLoaded.points().at(1);
        P2_CHECK("P2P-21 CSV writeProtected", p.writeProtected);
        P2_CHECK("P2P-22 CSV note", p.writeProtectNote == "safety");
        P2_CHECK("P2P-23 CSV interlock enabled", p.interlockEnabled);
        P2_CHECK("P2P-24 CSV interlock id", p.interlockPointId == 1);
        P2_CHECK("P2P-25 CSV interlock range",
                 qFuzzyCompare(p.interlockMin, 1.5) && qFuzzyCompare(p.interlockMax, 9.5));
    }

    // Old CSV header (without new columns) keeps defaults.
    const QByteArray oldCsv =
        "id,name,serverAddress,registerType,address,count,dataType,scale,offset,unit,alarmLow,alarmHigh,archiveEnabled,archiveIntervalSec\n"
        "1,Legacy,1,3,7,1,uint16,1,0,,0,65535,1,5\n";
    const QString oldCsvPath = dir.filePath("old.csv");
    P2_CHECK("P2P-26 write old CSV", writeFile(oldCsvPath, oldCsv));
    PointModel oldCsvModel;
    P2_CHECK("P2P-27 import old CSV",
             oldCsvModel.importFromCsv(oldCsvPath, false, &imported, &error));
    P2_CHECK("P2P-28 old CSV defaults to unprotected",
             oldCsvModel.points().size() == 1 && !oldCsvModel.points().first().writeProtected);
    P2_CHECK("P2P-29 old CSV defaults to no interlock",
             oldCsvModel.points().size() == 1 && !oldCsvModel.points().first().interlockEnabled);

    // Invalid interlock point id disables the interlock instead of failing the row.
    const QByteArray badCsv =
        "name,serverAddress,registerType,address,count,dataType,interlockEnabled,interlockPointId,interlockMin,interlockMax\n"
        "Bad,1,3,9,1,uint16,1,abc,0,10\n"
        "Inverted,1,3,10,1,uint16,1,1,10,0\n";
    const QString badCsvPath = dir.filePath("bad.csv");
    P2_CHECK("P2P-30 write invalid interlock CSV", writeFile(badCsvPath, badCsv));
    PointModel badModel;
    P2_CHECK("P2P-31 invalid interlock CSV still imports",
             badModel.importFromCsv(badCsvPath, false, &imported, &error));
    P2_CHECK("P2P-32 invalid id disables interlock",
             badModel.points().size() >= 1 && !badModel.points().first().interlockEnabled);
    P2_CHECK("P2P-33 inverted range disables interlock",
             badModel.points().size() >= 2 && !badModel.points().at(1).interlockEnabled);
}

static void testReportScheduler()
{
    printf("\n=== P2R scheduled reports / database backup ===\n");

    const QDateTime fixed(QDate(2026, 1, 2), QTime(3, 4, 5));
    const QString reportName = ReportScheduler::reportFileName("out", fixed);
    P2_CHECK("P2R-01 report file name uses timestamp", reportName.endsWith("report_20260102_030405.md"));
    P2_CHECK("P2R-02 report file name uses directory", reportName.contains("out"));
    const QString backupName = ReportScheduler::backupFileName("out", "/data/history.db", fixed);
    P2_CHECK("P2R-03 backup file name keeps base name", backupName.contains("history_20260102_030405"));
    P2_CHECK("P2R-04 backup file name ends with .db", backupName.endsWith(".db"));

    // writeTextFile creates nested directories.
    QTemporaryDir dir;
    const QString textPath = dir.filePath("a/b/report.md");
    QString error;
    P2_CHECK("P2R-05 write text file creates directories",
             ReportScheduler::writeTextFile(textPath, "hello", &error));
    P2_CHECK("P2R-06 written content matches", readFile(textPath) == "hello");

    // rotateFiles keeps the newest N.
    const QString rotateDir = dir.filePath("rotate");
    QDir().mkpath(rotateDir);
    for (int i = 0; i < 5; ++i) {
        const QString name = QStringLiteral("report_2026010%1_000000.md").arg(i + 1);
        writeFile(QDir(rotateDir).absoluteFilePath(name), "x");
    }
    QStringList removed;
    const int deleted = ReportScheduler::rotateFiles(rotateDir, "report_", ".md", 2, &removed);
    P2_CHECK("P2R-07 rotation deletes overflow", deleted == 3);
    P2_CHECK("P2R-08 rotation reports removed files", removed.size() == 3);
    const QFileInfoList remaining = QDir(rotateDir).entryInfoList(
        QStringList{"report_*.md"}, QDir::Files, QDir::Name);
    P2_CHECK("P2R-09 rotation keeps requested count", remaining.size() == 2);
    P2_CHECK("P2R-10 rotation keeps newest files",
             remaining.size() == 2 && remaining.first().fileName().contains("20260104")
                 && remaining.last().fileName().contains("20260105"));
    P2_CHECK("P2R-11 keep <= 0 disables rotation",
             ReportScheduler::rotateFiles(rotateDir, "report_", ".md", 0) == 0);

    // Markdown report content.
    QVector<PointHistoryRecord> records;
    auto addRecord = [&](int pointId, const QString &name, double value,
                                int quality, int seconds) {
        PointHistoryRecord record;
        record.deviceId = 1;
        record.pointId = pointId;
        record.pointName = name;
        record.value = value;
        record.quality = quality;
        record.timestamp = fixed.addSecs(seconds);
        records.append(record);
    };
    addRecord(1, "Point A", 10.0, 0, 0);
    addRecord(1, "Point A", 20.0, 0, 60);
    addRecord(1, "Point A", 30.0, 1, 120);
    addRecord(2, "Point B", 5.0, 0, 30);

    const QString markdown = ReportScheduler::buildReportMarkdown(
        "Report", fixed.addSecs(-3600), fixed.addSecs(3600), records, nullptr);
    P2_CHECK("P2R-12 report contains title", markdown.contains("# Report"));
    P2_CHECK("P2R-13 report contains point A", markdown.contains("Point A"));
    P2_CHECK("P2R-14 report contains point B", markdown.contains("Point B"));
    P2_CHECK("P2R-15 report contains minimum", markdown.contains("| 10 |"));
    P2_CHECK("P2R-16 report contains maximum", markdown.contains("| 30 |"));
    P2_CHECK("P2R-17 report contains average", markdown.contains("| 20 |"));
    P2_CHECK("P2R-18 report counts samples", markdown.contains("| 3 |"));
    P2_CHECK("P2R-19 report counts bad samples", markdown.contains("| 1 |"));
    P2_CHECK("P2R-20 report notes missing alarm database",
             markdown.contains("Alarm database is not available"));

    const QString emptyReport = ReportScheduler::buildReportMarkdown(
        "Empty", fixed, fixed, {}, nullptr);
    P2_CHECK("P2R-21 empty report explains no data",
             emptyReport.contains("No data in this period"));

    QVector<AlarmEvent> events;
    AlarmEvent critical;
    critical.timestamp = fixed;
    critical.ruleName = "High";
    critical.message = "too high";
    critical.severity = AlarmSeverity::Critical;
    critical.acknowledged = false;
    AlarmEvent info;
    info.timestamp = fixed;
    info.ruleName = "Info";
    info.message = "just info";
    info.severity = AlarmSeverity::Info;
    info.acknowledged = true;
    events << critical << info;
    const QString alarmReport = ReportScheduler::buildReportMarkdown(
        "Alarms", fixed.addSecs(-60), fixed.addSecs(60), {}, &events);
    P2_CHECK("P2R-22 report counts critical alarms", alarmReport.contains("Critical: 1"));
    P2_CHECK("P2R-23 report counts info alarms", alarmReport.contains("Info: 1"));
    P2_CHECK("P2R-24 report counts unacknowledged alarms",
             alarmReport.contains("Unacknowledged: 1"));

    // Database backup uses VACUUM INTO and produces a readable database.
    const QString dbPath = dir.filePath("history.db");
    HistoryData history;
    P2_CHECK("P2R-25 open source history db", history.openDatabase(dbPath));
    PointHistoryRecord record;
    record.deviceId = 1;
    record.pointId = 1;
    record.pointName = "Point A";
    record.value = 42.0;
    record.quality = 0;
    record.timestamp = QDateTime::currentDateTime();
    P2_CHECK("P2R-26 insert source record", history.addPointRecord(record));

    const QString targetPath = dir.filePath("backup.db");
    P2_CHECK("P2R-27 backup database succeeds",
             ReportScheduler::backupDatabase(dbPath, targetPath, &error));
    P2_CHECK("P2R-28 backup file exists", QFileInfo::exists(targetPath));
    P2_CHECK("P2R-29 backup file is not empty", QFileInfo(targetPath).size() > 0);
    {
        HistoryData restored;
        P2_CHECK("P2R-30 backup opens as SQLite database", restored.openDatabase(targetPath));
        P2_CHECK("P2R-31 backup contains the record", restored.totalPointRecords() == 1);
    }
    P2_CHECK("P2R-32 missing source is rejected",
             !ReportScheduler::backupDatabase(dir.filePath("missing.db"),
                                              dir.filePath("missing_backup.db"), &error));

    QStringList created;
    QStringList skipped;
    const QStringList sources{dbPath, dir.filePath("missing.db")};
    P2_CHECK("P2R-33 batch backup succeeds with one missing source",
             ReportScheduler::backupDatabases(sources, dir.filePath("backups"), fixed,
                                              &created, &skipped, &error));
    P2_CHECK("P2R-34 batch backup created one file", created.size() == 1);
    P2_CHECK("P2R-35 batch backup skipped missing source", skipped.size() == 1);
    P2_CHECK("P2R-36 batch backup file exists", QFileInfo::exists(created.value(0)));

    // Scheduler start / stop and manual run.
    ReportScheduler scheduler;
    SchedulerConfig config;
    config.reportEnabled = true;
    config.reportIntervalMinutes = 1;
    config.reportDir = dir.filePath("reports");
    config.reportRetention = 5;
    config.backupEnabled = true;
    config.backupIntervalMinutes = 1;
    config.backupDir = dir.filePath("auto_backups");
    config.backupRetention = 5;
    config.databasePaths = QStringList{dbPath};
    scheduler.setConfig(config);
    scheduler.setReportSources(&history, nullptr);
    scheduler.start();
    P2_CHECK("P2R-37 scheduler starts when enabled", scheduler.isRunning());
    scheduler.stop();
    P2_CHECK("P2R-38 scheduler stops", !scheduler.isRunning());

    P2_CHECK("P2R-39 manual report succeeds", scheduler.runReportsNow());
    P2_CHECK("P2R-40 manual report records time", scheduler.lastReportTime().isValid());
    P2_CHECK("P2R-41 manual report file exists",
             QFileInfo::exists(scheduler.lastReportPath()));
    const QString reportBody = QString::fromUtf8(readFile(scheduler.lastReportPath()));
    P2_CHECK("P2R-42 manual report contains point data", reportBody.contains("Point A"));

    P2_CHECK("P2R-43 manual backup succeeds", scheduler.runBackupNow());
    P2_CHECK("P2R-44 manual backup records time", scheduler.lastBackupTime().isValid());
    P2_CHECK("P2R-45 manual backup summary is not empty",
             !scheduler.lastBackupSummary().isEmpty());

    SchedulerConfig emptyConfig;
    ReportScheduler emptyScheduler;
    emptyScheduler.setConfig(emptyConfig);
    P2_CHECK("P2R-46 report without directory fails", !emptyScheduler.runReportsNow());
    P2_CHECK("P2R-47 backup without directory fails", !emptyScheduler.runBackupNow());
}

static void testSiteConfigBundle()
{
    printf("\n=== P2S site configuration bundle ===\n");

    P2_CHECK("P2S-01 format name", SiteConfigBundle::formatName() == "fieldlink-site-config");
    P2_CHECK("P2S-02 format version", SiteConfigBundle::formatVersion() == 1);
    P2_CHECK("P2S-03 sha256 of empty data",
             SiteConfigBundle::sha256Hex(QByteArray())
                 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    P2_CHECK("P2S-04 simple relative path accepted",
             SiteConfigBundle::safeRelativePath("points.json"));
    P2_CHECK("P2S-05 nested relative path accepted",
             SiteConfigBundle::safeRelativePath("profiles/site.ini"));
    P2_CHECK("P2S-06 parent traversal rejected",
             !SiteConfigBundle::safeRelativePath("../evil.json"));
    P2_CHECK("P2S-07 nested traversal rejected",
             !SiteConfigBundle::safeRelativePath("a/../../evil.json"));
    P2_CHECK("P2S-08 absolute POSIX path rejected",
             !SiteConfigBundle::safeRelativePath("/etc/passwd"));
    P2_CHECK("P2S-09 absolute Windows path rejected",
             !SiteConfigBundle::safeRelativePath("C:/Windows/system.ini"));
    P2_CHECK("P2S-10 UNC path rejected",
             !SiteConfigBundle::safeRelativePath("//server/share/file"));
    P2_CHECK("P2S-11 current-directory segment rejected",
             !SiteConfigBundle::safeRelativePath("a/./b"));
    P2_CHECK("P2S-12 empty path rejected", !SiteConfigBundle::safeRelativePath(""));
    P2_CHECK("P2S-13 whitespace-padded path rejected",
             !SiteConfigBundle::safeRelativePath(" points.json"));
    P2_CHECK("P2S-14 control character rejected",
             !SiteConfigBundle::safeRelativePath(QString("a") + QChar(0x01) + "b"));
    P2_CHECK("P2S-15 double separator rejected",
             !SiteConfigBundle::safeRelativePath("a//b"));

    QTemporaryDir srcDir;
    QTemporaryDir dstDir;
    QTemporaryDir settingsDir;
    const QString pointsPath = srcDir.filePath("points.json");
    const QString templatesPath = srcDir.filePath("templates.json");
    P2_CHECK("P2S-16 write source points", writeFile(pointsPath, "[{\"name\":\"A\"}]"));
    P2_CHECK("P2S-17 write source templates", writeFile(templatesPath, "[]"));

    const QString srcSettingsPath = settingsDir.filePath("src.ini");
    QSettings srcSettings(srcSettingsPath, QSettings::IniFormat);
    srcSettings.setValue("devices/1/name", "PLC");
    srcSettings.setValue("ui/theme", "dark");
    srcSettings.setValue("report/retention", 30);
    srcSettings.setValue("backup/enabled", true);
    srcSettings.sync();

    QStringList skipped;
    QString error;
    const QStringList sourceFiles{"points.json", "templates.json", "missing.json"};
    const QByteArray bundle = SiteConfigBundle::buildBundle(
        srcDir.path(), sourceFiles, srcSettings, "test-version", &skipped, &error);
    P2_CHECK("P2S-18 bundle is not empty", !bundle.isEmpty());
    P2_CHECK("P2S-19 missing file is skipped", skipped.size() == 1 && skipped.first() == "missing.json");

    const QJsonObject root = QJsonDocument::fromJson(bundle).object();
    P2_CHECK("P2S-20 bundle format field", root.value("format").toString() == "fieldlink-site-config");
    P2_CHECK("P2S-21 bundle version field", root.value("version").toInt() == 1);
    P2_CHECK("P2S-22 bundle records app version", root.value("appVersion").toString() == "test-version");
    P2_CHECK("P2S-23 bundle contains two files", root.value("files").toArray().size() == 2);
    P2_CHECK("P2S-24 bundle contains settings", root.value("settings").isObject());
    P2_CHECK("P2S-25 bundle keeps string setting",
             root.value("settings").toObject().value("devices/1/name").toString() == "PLC");
    P2_CHECK("P2S-26 bundle keeps boolean setting",
             root.value("settings").toObject().value("backup/enabled").toBool());

    const QJsonObject firstEntry = root.value("files").toArray().at(0).toObject();
    P2_CHECK("P2S-27 bundle file has checksum",
             firstEntry.value("sha256").toString().size() == 64);
    P2_CHECK("P2S-28 bundle checksum matches decoded data",
             SiteConfigBundle::sha256Hex(
                 QByteArray::fromBase64(firstEntry.value("data").toString().toLatin1()))
                 == firstEntry.value("sha256").toString());

    const QString bundlePath = settingsDir.filePath("site.json");
    P2_CHECK("P2S-29 write bundle file",
             SiteConfigBundle::writeBundleFile(bundlePath, bundle, &error));
    QByteArray readBack;
    P2_CHECK("P2S-30 read bundle file",
             SiteConfigBundle::readBundleFile(bundlePath, &readBack, &error));
    P2_CHECK("P2S-31 bundle round-trip is byte-identical", readBack == bundle);
    P2_CHECK("P2S-32 valid bundle passes validation",
             SiteConfigBundle::validateBundle(readBack, &error));

    // Validation failures.
    {
        QJsonObject bad = root;
        bad["format"] = "other";
        P2_CHECK("P2S-33 wrong format rejected",
                 !SiteConfigBundle::validateBundle(QJsonDocument(bad).toJson(), &error));
    }
    {
        QJsonObject bad = root;
        bad["version"] = 99;
        P2_CHECK("P2S-34 unsupported version rejected",
                 !SiteConfigBundle::validateBundle(QJsonDocument(bad).toJson(), &error));
    }
    {
        QJsonObject bad = root;
        QJsonArray files = bad.value("files").toArray();
        QJsonObject entry = files.at(0).toObject();
        entry["data"] = QString::fromLatin1(QByteArray("HACKED").toBase64());
        files[0] = entry;
        bad["files"] = files;
        P2_CHECK("P2S-35 checksum mismatch rejected",
                 !SiteConfigBundle::validateBundle(QJsonDocument(bad).toJson(), &error));
    }
    {
        QJsonObject bad = root;
        QJsonArray files = bad.value("files").toArray();
        QJsonObject entry = files.at(0).toObject();
        entry["path"] = "../evil.json";
        files[0] = entry;
        bad["files"] = files;
        P2_CHECK("P2S-36 traversal in bundle rejected",
                 !SiteConfigBundle::validateBundle(QJsonDocument(bad).toJson(), &error));
    }
    {
        QJsonObject bad = root;
        QJsonArray files = bad.value("files").toArray();
        files.append(files.at(0));
        bad["files"] = files;
        P2_CHECK("P2S-37 duplicate file path rejected",
                 !SiteConfigBundle::validateBundle(QJsonDocument(bad).toJson(), &error));
    }
    {
        QJsonObject bad = root;
        bad["settings"] = QJsonArray();
        P2_CHECK("P2S-38 non-object settings rejected",
                 !SiteConfigBundle::validateBundle(QJsonDocument(bad).toJson(), &error));
    }
    {
        const QByteArray garbage = "not json";
        P2_CHECK("P2S-39 garbage bundle rejected",
                 !SiteConfigBundle::validateBundle(garbage, &error));
    }
    {
        QStringList unsafeSkipped;
        QString unsafeError;
        const QByteArray unsafe = SiteConfigBundle::buildBundle(
            srcDir.path(), QStringList{"../evil.json"}, srcSettings, "x",
            &unsafeSkipped, &unsafeError);
        P2_CHECK("P2S-40 export rejects unsafe path", unsafe.isEmpty());
        P2_CHECK("P2S-41 export reports unsafe path error", !unsafeError.isEmpty());
    }

    // Apply: existing file is backed up, new content + settings are applied.
    const QString dstPoints = dstDir.filePath("points.json");
    P2_CHECK("P2S-42 seed destination file", writeFile(dstPoints, "OLD"));
    const QString dstSettingsPath = settingsDir.filePath("dst.ini");
    QSettings dstSettings(dstSettingsPath, QSettings::IniFormat);
    QStringList written;
    QStringList warnings;
    P2_CHECK("P2S-43 apply bundle succeeds",
             SiteConfigBundle::applyBundle(bundle, dstDir.path(), dstSettings,
                                           &written, &warnings, &error));
    P2_CHECK("P2S-44 apply writes both files", written.size() == 2);
    P2_CHECK("P2S-45 applied file content matches",
             readFile(dstPoints) == "[{\"name\":\"A\"}]");
    P2_CHECK("P2S-46 applied templates file exists",
             QFileInfo::exists(dstDir.filePath("templates.json")));
    P2_CHECK("P2S-47 applied settings string",
             dstSettings.value("devices/1/name").toString() == "PLC");
    P2_CHECK("P2S-48 applied settings bool", dstSettings.value("backup/enabled").toBool());
    P2_CHECK("P2S-49 applied settings int",
             dstSettings.value("report/retention").toInt() == 30);
    const QString backupFile = findBackupFile(dstDir.filePath("config_backups"), "points.json");
    P2_CHECK("P2S-50 overwritten file was backed up", !backupFile.isEmpty());
    P2_CHECK("P2S-51 backup keeps original content", readFile(backupFile) == "OLD");

    // Invalid bundle must not touch existing files.
    {
        QJsonObject bad = root;
        QJsonArray files = bad.value("files").toArray();
        QJsonObject entry = files.at(0).toObject();
        entry["data"] = QString::fromLatin1(QByteArray("HACKED").toBase64());
        files[0] = entry;
        bad["files"] = files;
        QStringList badWritten;
        QStringList badWarnings;
        P2_CHECK("P2S-52 invalid bundle apply fails",
                 !SiteConfigBundle::applyBundle(QJsonDocument(bad).toJson(), dstDir.path(),
                                                dstSettings, &badWritten, &badWarnings, &error));
        P2_CHECK("P2S-53 invalid bundle leaves file untouched",
                 readFile(dstPoints) == "[{\"name\":\"A\"}]");
        P2_CHECK("P2S-54 invalid bundle writes nothing", badWritten.isEmpty());
    }

    // Missing settings produces a warning but still applies files.
    {
        QJsonObject noSettings = root;
        noSettings.remove("settings");
        const QString thirdDir = dstDir.filePath("third");
        QSettings thirdSettings(settingsDir.filePath("third.ini"), QSettings::IniFormat);
        QStringList thirdWritten;
        QStringList thirdWarnings;
        P2_CHECK("P2S-55 bundle without settings applies",
                 SiteConfigBundle::applyBundle(QJsonDocument(noSettings).toJson(), thirdDir,
                                               thirdSettings, &thirdWritten, &thirdWarnings,
                                               &error));
        P2_CHECK("P2S-56 bundle without settings warns", thirdWarnings.size() == 1);
        P2_CHECK("P2S-57 bundle without settings still writes files", thirdWritten.size() == 2);
    }
}

void runP2FeatureTests()
{
    testWriteGuard();
    testPointPersistence();
    testReportScheduler();
    testSiteConfigBundle();
}
