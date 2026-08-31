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

struct AlarmRule {
    int id;
    QString name;
    bool enabled;
    int serverAddress;
    int registerType;
    int address;
    AlarmCondition condition;
    double threshold1;
    double threshold2;
    AlarmSeverity severity;
    QString message;
    int debounceMs;
    bool acknowledged;
};

struct AlarmEvent {
    qint64 id;
    QDateTime timestamp;
    int ruleId;
    QString ruleName;
    AlarmSeverity severity;
    QString message;
    double value;
    bool acknowledged;
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
