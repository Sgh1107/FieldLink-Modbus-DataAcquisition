#ifndef ALARMMANAGER_H
#define ALARMMANAGER_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QDateTime>
#include <QMap>

enum class AlarmCondition {
    GreaterThan,
    LessThan,
    Equal,
    NotEqual,
    InRange,
    OutOfRange,
    BitSet,
    BitClear
};

enum class AlarmSeverity {
    Info,
    Warning,
    Critical
};

// 标量成员一律带默认值：结构体常以 `AlarmRule r;` 声明后逐字段赋值，
// 漏设成员会留下未定值，被后续条件判断读到（Valgrind 会报
// Conditional jump depends on uninitialised value）。
struct AlarmRule {
    int id = 0;
    QString name;
    bool enabled = true;
    int serverAddress = 1;
    int registerType = 0;
    int address = 0;
    AlarmCondition condition = AlarmCondition::GreaterThan;
    double threshold1 = 0.0;
    double threshold2 = 0.0;
    AlarmSeverity severity = AlarmSeverity::Warning;
    QString message;
    int debounceMs = 0;
    bool acknowledged = false;
};

struct AlarmEvent {
    qint64 id = 0;
    QDateTime timestamp;
    int ruleId = 0;
    QString ruleName;
    AlarmSeverity severity = AlarmSeverity::Warning;
    QString message;
    double value = 0.0;
    bool acknowledged = false;
    QDateTime acknowledgedTime;
};

class AlarmManager : public QObject
{
    Q_OBJECT

public:
    explicit AlarmManager(QObject *parent = nullptr);

    int addRule(const AlarmRule &rule);
    void removeRule(int ruleId);
    void updateRule(const AlarmRule &rule);
    void setRuleEnabled(int ruleId, bool enabled);
    AlarmRule getRule(int ruleId) const;
    QVector<AlarmRule> allRules() const;

    void checkValue(int serverAddress, int registerType, int address, double value);
    void acknowledgeAlarm(qint64 eventId);
    void acknowledgeAll();
    void clearHistory();

    QVector<AlarmEvent> activeAlarms() const;
    QVector<AlarmEvent> alarmHistory(int maxCount = 100) const;
    int activeAlarmCount() const;

signals:
    void alarmTriggered(const AlarmEvent &event);
    void alarmCleared(int ruleId);
    void alarmAcknowledged(qint64 eventId);

private:
    bool evaluateCondition(const AlarmRule &rule, double value) const;

    QVector<AlarmRule> m_rules;
    QVector<AlarmEvent> m_history;
    QMap<int, bool> m_activeStates;
    QMap<int, QDateTime> m_lastTriggerTime;
    int m_nextRuleId;
    qint64 m_nextEventId;
};

#endif // ALARMMANAGER_H
