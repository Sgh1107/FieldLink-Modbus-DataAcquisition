#include "alarmmanager.h"
#include <QMap>

AlarmManager::AlarmManager(QObject *parent)
    : QObject(parent)
    , m_nextRuleId(1)
    , m_nextEventId(1)
{
}

int AlarmManager::addRule(const AlarmRule &rule)
{
    AlarmRule r = rule;
    r.id = m_nextRuleId++;
    r.acknowledged = false;
    m_rules.append(r);
    m_activeStates[r.id] = false;
    return r.id;
}

void AlarmManager::removeRule(int ruleId)
{
    for (int i = 0; i < m_rules.size(); ++i) {
        if (m_rules[i].id == ruleId) {
            m_rules.removeAt(i);
            m_activeStates.remove(ruleId);
            m_lastTriggerTime.remove(ruleId);
            break;
        }
    }
}

void AlarmManager::updateRule(const AlarmRule &rule)
{
    for (auto &r : m_rules) {
        if (r.id == rule.id) {
            r = rule;
            break;
        }
    }
}

void AlarmManager::setRuleEnabled(int ruleId, bool enabled)
{
    for (auto &r : m_rules) {
        if (r.id == ruleId) {
            r.enabled = enabled;
            if (!enabled) {
                m_activeStates[ruleId] = false;
                emit alarmCleared(ruleId);
            }
            break;
        }
    }
}

AlarmRule AlarmManager::getRule(int ruleId) const
{
    for (const auto &r : m_rules) {
        if (r.id == ruleId)
            return r;
    }
    return AlarmRule();
}

QVector<AlarmRule> AlarmManager::allRules() const
{
    return m_rules;
}

void AlarmManager::checkValue(int serverAddress, int registerType, int address, double value)
{
    for (const auto &rule : m_rules) {
        if (!rule.enabled) continue;
        if (rule.serverAddress != serverAddress) continue;
        if (rule.registerType != registerType) continue;
        if (rule.address != address) continue;

        bool triggered = evaluateCondition(rule, value);
        bool wasActive = m_activeStates.value(rule.id, false);

        if (triggered && !wasActive) {
            if (rule.debounceMs > 0) {
                QDateTime lastTrigger = m_lastTriggerTime.value(rule.id);
                if (lastTrigger.isValid() &&
                    lastTrigger.msecsTo(QDateTime::currentDateTime()) < rule.debounceMs) {
                    continue;
                }
            }

            m_activeStates[rule.id] = true;
            m_lastTriggerTime[rule.id] = QDateTime::currentDateTime();

            AlarmEvent event;
            event.id = m_nextEventId++;
            event.timestamp = QDateTime::currentDateTime();
            event.ruleId = rule.id;
            event.ruleName = rule.name;
            event.severity = rule.severity;
            event.message = rule.message.isEmpty()
                ? QString("Alarm: %1, Value: %2").arg(rule.name).arg(value)
                : rule.message;
            event.value = value;
            event.acknowledged = false;

            m_history.append(event);
            emit alarmTriggered(event);
        }
        else if (!triggered && wasActive) {
            m_activeStates[rule.id] = false;
            emit alarmCleared(rule.id);
        }
    }
}

void AlarmManager::acknowledgeAlarm(qint64 eventId)
{
    for (auto &event : m_history) {
        if (event.id == eventId) {
            event.acknowledged = true;
            event.acknowledgedTime = QDateTime::currentDateTime();
            emit alarmAcknowledged(eventId);
            break;
        }
    }
}

void AlarmManager::acknowledgeAll()
{
    for (auto &event : m_history) {
        if (!event.acknowledged) {
            event.acknowledged = true;
            event.acknowledgedTime = QDateTime::currentDateTime();
            emit alarmAcknowledged(event.id);
        }
    }
}

void AlarmManager::clearHistory()
{
    m_history.clear();
}

QVector<AlarmEvent> AlarmManager::activeAlarms() const
{
    QVector<AlarmEvent> result;
    for (const auto &event : m_history) {
        if (!event.acknowledged && m_activeStates.value(event.ruleId, false))
            result.append(event);
    }
    return result;
}

QVector<AlarmEvent> AlarmManager::alarmHistory(int maxCount) const
{
    if (m_history.size() <= maxCount)
        return m_history;
    return m_history.mid(m_history.size() - maxCount);
}

int AlarmManager::activeAlarmCount() const
{
    int count = 0;
    for (auto it = m_activeStates.constBegin(); it != m_activeStates.constEnd(); ++it) {
        if (it.value()) ++count;
    }
    return count;
}

bool AlarmManager::evaluateCondition(const AlarmRule &rule, double value) const
{
    switch (rule.condition) {
    case AlarmCondition::GreaterThan:
        return value > rule.threshold1;
    case AlarmCondition::LessThan:
        return value < rule.threshold1;
    case AlarmCondition::Equal:
        return qFuzzyCompare(value, rule.threshold1);
    case AlarmCondition::NotEqual:
        return !qFuzzyCompare(value, rule.threshold1);
    case AlarmCondition::InRange:
        return value >= rule.threshold1 && value <= rule.threshold2;
    case AlarmCondition::OutOfRange:
        return value < rule.threshold1 || value > rule.threshold2;
    case AlarmCondition::BitSet:
        return (static_cast<int>(value) & (1 << static_cast<int>(rule.threshold1))) != 0;
    case AlarmCondition::BitClear:
        return (static_cast<int>(value) & (1 << static_cast<int>(rule.threshold1))) == 0;
    }
    return false;
}
