#include "reportscheduler.h"

#include "alarmmanager.h"
#include "alarmrepository.h"
#include "historydata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

namespace {

QString escapeSqlLiteral(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    return escaped;
}

QString markdownCell(QString value)
{
    value.replace(QLatin1Char('|'), QLatin1String("\\|"));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return value;
}

QString severityName(AlarmSeverity severity)
{
    switch (severity) {
    case AlarmSeverity::Info:
        return QCoreApplication::translate("ReportScheduler", "Info");
    case AlarmSeverity::Warning:
        return QCoreApplication::translate("ReportScheduler", "Warning");
    case AlarmSeverity::Critical:
        return QCoreApplication::translate("ReportScheduler", "Critical");
    }
    return QCoreApplication::translate("ReportScheduler", "Unknown");
}

} // namespace

ReportScheduler::ReportScheduler(QObject *parent)
    : QObject(parent)
{
    m_reportTimer = new QTimer(this);
    m_reportTimer->setSingleShot(false);
    connect(m_reportTimer, &QTimer::timeout, this, &ReportScheduler::onReportTimeout);

    m_backupTimer = new QTimer(this);
    m_backupTimer->setSingleShot(false);
    connect(m_backupTimer, &QTimer::timeout, this, &ReportScheduler::onBackupTimeout);
}

void ReportScheduler::setConfig(const SchedulerConfig &config)
{
    m_config = config;
    if (isRunning())
        start();   // 重新按新周期启动（start 内部会先 stop）
}

SchedulerConfig ReportScheduler::config() const
{
    return m_config;
}

void ReportScheduler::setReportSources(HistoryData *history, AlarmRepository *alarms)
{
    m_history = history;
    m_alarms = alarms;
}

void ReportScheduler::start()
{
    stop();
    if (m_config.reportEnabled && m_config.reportIntervalMinutes > 0) {
        m_reportTimer->start(qMax(1, m_config.reportIntervalMinutes) * 60 * 1000);
    }
    if (m_config.backupEnabled && m_config.backupIntervalMinutes > 0) {
        m_backupTimer->start(qMax(1, m_config.backupIntervalMinutes) * 60 * 1000);
    }
}

void ReportScheduler::stop()
{
    m_reportTimer->stop();
    m_backupTimer->stop();
}

bool ReportScheduler::isRunning() const
{
    return m_reportTimer->isActive() || m_backupTimer->isActive();
}

QDateTime ReportScheduler::lastReportTime() const
{
    return m_lastReportTime;
}

QDateTime ReportScheduler::lastBackupTime() const
{
    return m_lastBackupTime;
}

QString ReportScheduler::lastReportPath() const
{
    return m_lastReportPath;
}

QString ReportScheduler::lastBackupSummary() const
{
    return m_lastBackupSummary;
}

void ReportScheduler::onReportTimeout()
{
    runReportsNow();
}

void ReportScheduler::onBackupTimeout()
{
    runBackupNow();
}

bool ReportScheduler::runReportsNow()
{
    if (m_config.reportDir.trimmed().isEmpty()) {
        emit error(QCoreApplication::translate("ReportScheduler",
                                               "Report output directory is not configured."));
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const int windowMinutes = qMax(1, m_config.reportIntervalMinutes);
    const QDateTime from = now.addSecs(-windowMinutes * 60);

    QVector<PointHistoryRecord> records;
    if (m_history && m_history->isOpen())
        records = m_history->queryPoints(from, now, -1, -1, 50000);

    QVector<AlarmEvent> eventBuffer;
    const QVector<AlarmEvent> *events = nullptr;
    if (m_alarms && m_alarms->isOpen()) {
        const QVector<AlarmEvent> all = m_alarms->loadEvents(1000);
        for (const AlarmEvent &event : all) {
            if (event.timestamp.isValid() && event.timestamp >= from && event.timestamp <= now)
                eventBuffer.append(event);
        }
        events = &eventBuffer;
    }

    const QString title = QCoreApplication::translate("ReportScheduler", "FieldLink Scheduled Report");
    const QString content = buildReportMarkdown(title, from, now, records, events);
    const QString path = reportFileName(m_config.reportDir, now);

    QString error;
    if (!writeTextFile(path, content, &error)) {
        emit this->error(QCoreApplication::translate("ReportScheduler",
                                                     "Failed to write report: %1").arg(error));
        return false;
    }

    rotateFiles(m_config.reportDir, QStringLiteral("report_"), QStringLiteral(".md"),
                m_config.reportRetention);

    m_lastReportTime = now;
    m_lastReportPath = path;
    emit reportGenerated(path);
    return true;
}

bool ReportScheduler::runBackupNow()
{
    if (m_config.backupDir.trimmed().isEmpty()) {
        emit error(QCoreApplication::translate("ReportScheduler",
                                               "Backup directory is not configured."));
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    QStringList created;
    QStringList skipped;
    QString error;
    if (!backupDatabases(m_config.databasePaths, m_config.backupDir, now, &created, &skipped, &error)) {
        emit this->error(QCoreApplication::translate("ReportScheduler",
                                                     "Database backup failed: %1").arg(error));
        return false;
    }

    // 每个源库按自己的文件名前缀独立轮换，避免不同库互相挤占保留名额。
    for (const QString &source : m_config.databasePaths) {
        const QString base = QFileInfo(source).completeBaseName();
        if (base.isEmpty())
            continue;
        rotateFiles(m_config.backupDir, base + QLatin1Char('_'), QStringLiteral(".db"),
                    m_config.backupRetention);
    }

    m_lastBackupTime = now;
    m_lastBackupSummary = QCoreApplication::translate(
                              "ReportScheduler", "%1 database(s) backed up, %2 skipped")
                              .arg(created.size())
                              .arg(skipped.size());
    emit backupCompleted(m_lastBackupSummary);
    return true;
}

QString ReportScheduler::reportFileName(const QString &dir, const QDateTime &now)
{
    return QDir(dir).absoluteFilePath(
        QStringLiteral("report_%1.md").arg(now.toString(QStringLiteral("yyyyMMdd_HHmmss"))));
}

QString ReportScheduler::backupFileName(const QString &dir, const QString &sourcePath,
                                        const QDateTime &now)
{
    const QString base = QFileInfo(sourcePath).completeBaseName();
    return QDir(dir).absoluteFilePath(
        QStringLiteral("%1_%2.db").arg(base, now.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
}

QString ReportScheduler::buildReportMarkdown(const QString &title, const QDateTime &from,
                                             const QDateTime &to,
                                             const QVector<PointHistoryRecord> &records,
                                             const QVector<AlarmEvent> *events)
{
    struct Stats {
        QString name;
        int deviceId = 0;
        int pointId = 0;
        int samples = 0;
        int badSamples = 0;
        double minValue = 0.0;
        double maxValue = 0.0;
        double sum = 0.0;
        double lastValue = 0.0;
        QDateTime lastTime;
    };

    QMap<QPair<int, int>, Stats> grouped;
    for (const PointHistoryRecord &record : records) {
        const QPair<int, int> key(record.deviceId, record.pointId);
        auto it = grouped.find(key);
        if (it == grouped.end()) {
            Stats stats;
            stats.name = record.pointName;
            stats.deviceId = record.deviceId;
            stats.pointId = record.pointId;
            stats.minValue = record.value;
            stats.maxValue = record.value;
            it = grouped.insert(key, stats);
        }
        Stats &stats = it.value();
        if (stats.name.isEmpty())
            stats.name = record.pointName;
        ++stats.samples;
        stats.sum += record.value;
        stats.minValue = qMin(stats.minValue, record.value);
        stats.maxValue = qMax(stats.maxValue, record.value);
        if (record.quality != 0)
            ++stats.badSamples;
        if (!stats.lastTime.isValid() || record.timestamp >= stats.lastTime) {
            stats.lastTime = record.timestamp;
            stats.lastValue = record.value;
        }
    }

    QString out;
    QTextStream stream(&out);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec("UTF-8");
#endif
    stream << "# " << title << "\n\n";
    stream << "- " << QCoreApplication::translate("ReportScheduler", "From") << ": "
           << from.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\n";
    stream << "- " << QCoreApplication::translate("ReportScheduler", "To") << ": "
           << to.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\n";
    stream << "- " << QCoreApplication::translate("ReportScheduler", "Point samples") << ": "
           << records.size() << "\n";
    stream << "- " << QCoreApplication::translate("ReportScheduler", "Points covered") << ": "
           << grouped.size() << "\n\n";

    stream << "## " << QCoreApplication::translate("ReportScheduler", "Point Statistics") << "\n\n";
    stream << "| " << QCoreApplication::translate("ReportScheduler", "Device")
           << " | " << QCoreApplication::translate("ReportScheduler", "Point")
           << " | " << QCoreApplication::translate("ReportScheduler", "Samples")
           << " | " << QCoreApplication::translate("ReportScheduler", "Min")
           << " | " << QCoreApplication::translate("ReportScheduler", "Max")
           << " | " << QCoreApplication::translate("ReportScheduler", "Average")
           << " | " << QCoreApplication::translate("ReportScheduler", "Last")
           << " | " << QCoreApplication::translate("ReportScheduler", "Bad samples")
           << " |\n";
    stream << "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        const Stats &stats = it.value();
        const double average = stats.samples > 0 ? stats.sum / stats.samples : 0.0;
        stream << "| " << stats.deviceId
               << " | " << markdownCell(stats.name.isEmpty()
                                            ? QStringLiteral("#%1").arg(stats.pointId)
                                            : stats.name)
               << " | " << stats.samples
               << " | " << QString::number(stats.minValue, 'g', 10)
               << " | " << QString::number(stats.maxValue, 'g', 10)
               << " | " << QString::number(average, 'g', 10)
               << " | " << QString::number(stats.lastValue, 'g', 10)
               << " | " << stats.badSamples
               << " |\n";
    }
    if (grouped.isEmpty()) {
        stream << "| - | " << QCoreApplication::translate("ReportScheduler", "No data in this period")
               << " | 0 | - | - | - | - | - |\n";
    }

    stream << "\n## " << QCoreApplication::translate("ReportScheduler", "Alarm Summary") << "\n\n";
    if (events) {
        int info = 0;
        int warning = 0;
        int critical = 0;
        int unacknowledged = 0;
        for (const AlarmEvent &event : *events) {
            switch (event.severity) {
            case AlarmSeverity::Info:
                ++info;
                break;
            case AlarmSeverity::Warning:
                ++warning;
                break;
            case AlarmSeverity::Critical:
                ++critical;
                break;
            }
            if (!event.acknowledged)
                ++unacknowledged;
        }
        stream << "- " << QCoreApplication::translate("ReportScheduler", "Total events") << ": "
               << events->size() << "\n";
        stream << "- " << severityName(AlarmSeverity::Critical) << ": " << critical << "\n";
        stream << "- " << severityName(AlarmSeverity::Warning) << ": " << warning << "\n";
        stream << "- " << severityName(AlarmSeverity::Info) << ": " << info << "\n";
        stream << "- " << QCoreApplication::translate("ReportScheduler", "Unacknowledged") << ": "
               << unacknowledged << "\n\n";

        if (!events->isEmpty()) {
            stream << "| " << QCoreApplication::translate("ReportScheduler", "Time")
                   << " | " << QCoreApplication::translate("ReportScheduler", "Rule")
                   << " | " << QCoreApplication::translate("ReportScheduler", "Severity")
                   << " | " << QCoreApplication::translate("ReportScheduler", "Message")
                   << " |\n";
            stream << "| --- | --- | --- | --- |\n";
            const int shown = qMin(50, events->size());
            for (int i = 0; i < shown; ++i) {
                const AlarmEvent &event = events->at(i);
                stream << "| " << event.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                       << " | " << markdownCell(event.ruleName)
                       << " | " << severityName(event.severity)
                       << " | " << markdownCell(event.message) << " |\n";
            }
        }
    } else {
        stream << "- " << QCoreApplication::translate("ReportScheduler",
                                                      "Alarm database is not available")
               << "\n";
    }

    stream.flush();
    return out;
}

bool ReportScheduler::writeTextFile(const QString &path, const QString &content, QString *error)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = QCoreApplication::translate("ReportScheduler",
                                                 "Cannot create directory: %1").arg(info.absolutePath());
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#endif
    out << content;
    out.flush();
    if (out.status() != QTextStream::Ok) {
        if (error)
            *error = QCoreApplication::translate("ReportScheduler", "Write failed");
        return false;
    }
    return true;
}

int ReportScheduler::rotateFiles(const QString &dir, const QString &prefix, const QString &suffix,
                                 int keep, QStringList *removed)
{
    if (keep <= 0)
        return 0;

    QDir directory(dir);
    if (!directory.exists())
        return 0;

    const QStringList filters{QStringLiteral("%1*%2").arg(prefix, suffix)};
    QFileInfoList files = directory.entryInfoList(filters, QDir::Files, QDir::Name);
    // 文件名以时间戳命名，按名称升序即时间升序；超出保留数的从最旧开始删。
    int deleted = 0;
    while (files.size() > keep) {
        const QFileInfo oldest = files.takeFirst();
        if (QFile::remove(oldest.absoluteFilePath())) {
            ++deleted;
            if (removed)
                removed->append(oldest.absoluteFilePath());
        }
    }
    return deleted;
}

bool ReportScheduler::backupDatabase(const QString &sourcePath, const QString &targetPath,
                                     QString *error)
{
    if (sourcePath.trimmed().isEmpty() || !QFileInfo::exists(sourcePath)) {
        if (error)
            *error = QCoreApplication::translate("ReportScheduler",
                                                 "Source database does not exist: %1").arg(sourcePath);
        return false;
    }

    const QFileInfo targetInfo(targetPath);
    if (!QDir().mkpath(targetInfo.absolutePath())) {
        if (error)
            *error = QCoreApplication::translate("ReportScheduler",
                                                 "Cannot create backup directory: %1")
                         .arg(targetInfo.absolutePath());
        return false;
    }
    // VACUUM INTO 要求目标文件不存在；同名文件（同一毫秒内重复触发）先清掉。
    if (QFileInfo::exists(targetPath))
        QFile::remove(targetPath);

    const QString connectionName = QStringLiteral("fieldlink_backup_%1")
                                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = false;
    QString errorText;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(sourcePath);
        if (!db.open()) {
            errorText = db.lastError().text();
        } else {
            QSqlQuery query(db);
            const QString sql = QStringLiteral("VACUUM INTO '%1'").arg(escapeSqlLiteral(targetPath));
            if (query.exec(sql) && QFileInfo::exists(targetPath)
                && QFileInfo(targetPath).size() > 0) {
                ok = true;
            } else {
                errorText = query.lastError().text().isEmpty()
                                ? QCoreApplication::translate("ReportScheduler", "VACUUM INTO failed")
                                : query.lastError().text();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);

    if (!ok) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(QFileInfo(sourcePath).fileName(), errorText);
        return false;
    }
    return true;
}

bool ReportScheduler::backupDatabases(const QStringList &sourcePaths, const QString &targetDir,
                                      const QDateTime &now, QStringList *created,
                                      QStringList *skipped, QString *error)
{
    if (targetDir.trimmed().isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("ReportScheduler", "Backup directory is empty");
        return false;
    }

    bool allOk = true;
    for (const QString &source : sourcePaths) {
        if (!QFileInfo::exists(source)) {
            if (skipped)
                skipped->append(source);
            continue;
        }
        const QString target = backupFileName(targetDir, source, now);
        QString oneError;
        if (!backupDatabase(source, target, &oneError)) {
            allOk = false;
            if (error)
                *error = oneError;
            continue;
        }
        if (created)
            created->append(target);
    }
    return allOk;
}
