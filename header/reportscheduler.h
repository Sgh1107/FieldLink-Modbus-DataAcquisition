#ifndef REPORTSCHEDULER_H
#define REPORTSCHEDULER_H

// reportscheduler.h
// 定时报表 + 数据库自动备份。
//
// 设计要点：
//   - 报表与备份的"纯逻辑"（文件名、保留期轮换、VACUUM INTO、Markdown 生成）
//     全部是静态函数，可在无 GUI 的测试里直接调用；
//   - 定时器只负责按配置触发，不把业务逻辑写进 GUI；
//   - 备份使用 SQLite `VACUUM INTO`：WAL 模式下也是一致性快照，且不会像
//     "直接拷 .db" 那样漏掉还在 .db-wal 里的最新写入；
//   - 报表/备份失败只发信号并记录，不影响采集主流程。

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;
class HistoryData;
class AlarmRepository;
struct PointHistoryRecord;
struct AlarmEvent;

struct SchedulerConfig {
    bool reportEnabled = false;
    int reportIntervalMinutes = 60;
    QString reportDir;
    int reportRetention = 30;          // <=0 表示不自动清理

    bool backupEnabled = false;
    int backupIntervalMinutes = 60;
    QString backupDir;
    int backupRetention = 14;          // <=0 表示不自动清理

    // 需要备份的数据库文件（绝对路径，允许不存在；不存在时跳过并记录）。
    QStringList databasePaths;
};

class ReportScheduler : public QObject
{
    Q_OBJECT

public:
    explicit ReportScheduler(QObject *parent = nullptr);

    void setConfig(const SchedulerConfig &config);
    SchedulerConfig config() const;

    // 数据源。未设置时 runReportsNow() 只生成"无数据"报表。
    void setReportSources(HistoryData *history, AlarmRepository *alarms);

    void start();
    void stop();
    bool isRunning() const;

    QDateTime lastReportTime() const;
    QDateTime lastBackupTime() const;
    QString lastReportPath() const;
    QString lastBackupSummary() const;

public slots:
    // 立即执行一次（手动按钮 / 定时器共用）。成功返回 true。
    bool runReportsNow();
    bool runBackupNow();

signals:
    void reportGenerated(const QString &filePath);
    void backupCompleted(const QString &summary);
    void error(const QString &message);

public:
    // ---------- 可独立测试的纯逻辑 ----------
    static QString reportFileName(const QString &dir, const QDateTime &now);
    static QString backupFileName(const QString &dir, const QString &sourcePath,
                                  const QDateTime &now);
    // 生成 Markdown 报表正文。alarms 可为空。
    static QString buildReportMarkdown(const QString &title, const QDateTime &from,
                                       const QDateTime &to,
                                       const QVector<PointHistoryRecord> &records,
                                       const QVector<AlarmEvent> *events);
    static bool writeTextFile(const QString &path, const QString &content, QString *error);
    // 按前缀轮换：只保留最新的 keep 个文件（keep <= 0 时不删除）。返回删除数量。
    static int rotateFiles(const QString &dir, const QString &prefix, const QString &suffix,
                           int keep, QStringList *removed = nullptr);
    // 用 VACUUM INTO 把 sourcePath 备份到 targetPath。targetPath 必须不存在。
    static bool backupDatabase(const QString &sourcePath, const QString &targetPath,
                               QString *error);
    // 批量备份；成功时 created 返回生成的文件列表。
    static bool backupDatabases(const QStringList &sourcePaths, const QString &targetDir,
                                const QDateTime &now, QStringList *created,
                                QStringList *skipped, QString *error);

private slots:
    void onReportTimeout();
    void onBackupTimeout();

private:
    SchedulerConfig m_config;
    QTimer *m_reportTimer = nullptr;
    QTimer *m_backupTimer = nullptr;
    HistoryData *m_history = nullptr;
    AlarmRepository *m_alarms = nullptr;
    QDateTime m_lastReportTime;
    QDateTime m_lastBackupTime;
    QString m_lastReportPath;
    QString m_lastBackupSummary;
};

#endif // REPORTSCHEDULER_H
