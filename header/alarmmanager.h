#ifndef ALARMMANAGER_H
#define ALARMMANAGER_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QDateTime>
#include <QMap>
#include <QHash>
#include <QQueue>
#include <QSet>

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

    // P2-67：报警历史条数上限。超过上限后自动丢弃最旧的事件，
    // 避免长期运行时内存无界增长。setMaxHistory(<=0) 视为使用默认值。
    int maxHistory() const;
    void setMaxHistory(int maxCount);
    static int defaultMaxHistory() { return 10000; }

signals:
    void alarmTriggered(const AlarmEvent &event);
    void alarmCleared(int ruleId);
    void alarmAcknowledged(qint64 eventId);

private:
    bool evaluateCondition(const AlarmRule &rule, double value) const;
    // 去抖判定：基准是"上次真正发出事件的时间"（m_lastTriggerTime），
    // 而不是"上次尝试触发的时间"，被抑制的事件不会重置窗口。
    bool isDebounced(const AlarmRule &rule, const QDateTime &now) const;
    void emitTriggered(const AlarmRule &rule, double value, const QDateTime &now);
    void appendHistory(const AlarmEvent &event);
    // 规则增删改后重建地址索引（serverAddress -> (registerType,address) -> 规则 id 列表）
    void rebuildIndex();
    static quint64 addressKey(int registerType, int address);

    QVector<int> m_ruleOrder;                            // 规则 id（保持添加顺序）
    QHash<int, AlarmRule> m_ruleMap;                      // 规则 id -> 规则定义
    QHash<int, QHash<quint64, QVector<int>>> m_addressIndex; // 从站 -> (类型,地址) -> 规则 id 列表
    QQueue<AlarmEvent> m_history;                         // 环形队列：出队 O(1)，条数严格受上限约束
    QMap<int, bool> m_activeStates;
    QMap<int, QDateTime> m_lastTriggerTime;               // 上次真正发出事件的时间（去抖基准）
    QSet<int> m_pendingNotifications;                     // 事件被去抖抑制、尚未告知用户的规则
    int m_nextRuleId;
    qint64 m_nextEventId;
    int m_maxHistory = 10000;
};

#endif // ALARMMANAGER_H
