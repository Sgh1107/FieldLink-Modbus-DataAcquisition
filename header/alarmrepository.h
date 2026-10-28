#ifndef ALARMREPOSITORY_H
#define ALARMREPOSITORY_H

// alarmrepository.h
// 报警规则 / 报警事件 / 报警活动状态的 SQLite 持久化层。
//
// 设计要点：
//   - 独立数据库文件（默认与 history.db 同目录的 alarms.db），不参与历史数据清理；
//   - 只做数据存取，不依赖 GUI / AlarmManager，便于单元测试；
//   - AlarmManager 通过 setRepository() 注入本类，所有增删改/确认/状态变化自动落库；
//   - 数据库不可用时只记录错误并返回 false，内存中的报警逻辑继续工作（降级不阻断）。

#include "alarmmanager.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

struct AlarmRuleState {
    int ruleId = 0;
    bool active = false;
    QDateTime lastTriggerTime;
};

class AlarmRepository
{
public:
    explicit AlarmRepository(const QString &dbPath = QString());
    ~AlarmRepository();

    bool open(const QString &dbPath);
    void close();
    bool isOpen() const;
    QString databasePath() const;
    QString lastError() const;

    // 规则
    bool saveRule(const AlarmRule &rule);
    bool deleteRule(int ruleId);                 // 同时删除该规则的事件与状态
    QVector<AlarmRule> loadRules();

    // 事件
    bool saveEvent(const AlarmEvent &event);
    bool updateEventAck(qint64 eventId, bool acknowledged, const QDateTime &acknowledgedTime);
    bool acknowledgeAllEvents();
    bool deleteAllEvents();
    QVector<AlarmEvent> loadEvents(int maxCount);

    // 活动状态（用于重启后恢复活跃报警，避免重复触发）
    bool saveState(int ruleId, bool active, const QDateTime &lastTriggerTime);
    bool deleteState(int ruleId);
    QVector<AlarmRuleState> loadStates();

    // 供 AlarmManager 计算下一个可用 ID（取规则/事件表最大值 + 1）
    int nextRuleId();
    qint64 nextEventId();

    int ruleCount();
    int eventCount();

private:
    bool ensureSchema();
    bool execPragma(const QString &sql);
    void setError(const QString &context, const QString &detail);
    void clearError();

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_dbPath;
    QString m_lastError;
    bool m_schemaReady = false;
};

#endif // ALARMREPOSITORY_H
