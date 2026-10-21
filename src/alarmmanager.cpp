#include "alarmmanager.h"
#include <QMap>
#include <QHash>
#include <QSet>
#include <cmath>

AlarmManager::AlarmManager(QObject *parent)
    : QObject(parent)
    , m_nextRuleId(1)
    , m_nextEventId(1)
{
}

quint64 AlarmManager::addressKey(int registerType, int address)
{
    // 高 32 位放寄存器类型，低 32 位放寄存器地址，避免不同类型/地址互相碰撞
    return (quint64(quint32(registerType)) << 32) | quint64(quint32(address));
}

void AlarmManager::rebuildIndex()
{
    m_addressIndex.clear();
    for (int id : m_ruleOrder) {
        auto it = m_ruleMap.constFind(id);
        if (it == m_ruleMap.constEnd())
            continue;
        const AlarmRule &r = it.value();
        m_addressIndex[r.serverAddress][addressKey(r.registerType, r.address)].append(id);
    }
}

int AlarmManager::addRule(const AlarmRule &rule)
{
    AlarmRule r = rule;
    r.id = m_nextRuleId++;
    r.acknowledged = false;
    m_ruleMap.insert(r.id, r);
    m_ruleOrder.append(r.id);
    m_activeStates[r.id] = false;
    rebuildIndex();
    return r.id;
}

void AlarmManager::removeRule(int ruleId)
{
    if (!m_ruleMap.contains(ruleId))
        return;

    m_ruleMap.remove(ruleId);
    m_ruleOrder.removeAll(ruleId);
    m_activeStates.remove(ruleId);
    m_lastTriggerTime.remove(ruleId);
    m_pendingNotifications.remove(ruleId);

    // P2-69：删除规则时一并清除其历史事件。
    // 否则"历史"页仍能看到该规则的报警，而"活跃"页永远看不到，自相矛盾。
    for (int i = m_history.size() - 1; i >= 0; --i) {
        if (m_history.at(i).ruleId == ruleId)
            m_history.removeAt(i);
    }

    rebuildIndex();
}

void AlarmManager::updateRule(const AlarmRule &rule)
{
    auto it = m_ruleMap.find(rule.id);
    if (it == m_ruleMap.end())
        return;

    it.value() = rule;

    // P2-70：规则被修改后（阈值/条件/去抖可能全变），重置去抖窗口与激活状态，
    // 否则旧规则留下的去抖时间会抑制新规则的首个事件。
    m_lastTriggerTime.remove(rule.id);
    m_pendingNotifications.remove(rule.id);
    m_activeStates[rule.id] = false;

    // 规则的地址/类型可能变化，索引需重建
    rebuildIndex();
}

void AlarmManager::setRuleEnabled(int ruleId, bool enabled)
{
    auto it = m_ruleMap.find(ruleId);
    if (it == m_ruleMap.end())
        return;

    it.value().enabled = enabled;
    if (!enabled) {
        const bool wasActive = m_activeStates.value(ruleId, false);
        m_activeStates[ruleId] = false;
        m_pendingNotifications.remove(ruleId);

        // P2-68：只有规则确实处于激活状态时才发 alarmCleared，
        // 否则下游会记录一条并不存在的"报警清除"。
        if (wasActive)
            emit alarmCleared(ruleId);
    }
}

AlarmRule AlarmManager::getRule(int ruleId) const
{
    auto it = m_ruleMap.constFind(ruleId);
    if (it != m_ruleMap.constEnd())
        return it.value();
    return AlarmRule();
}

QVector<AlarmRule> AlarmManager::allRules() const
{
    QVector<AlarmRule> result;
    result.reserve(m_ruleOrder.size());
    for (int id : m_ruleOrder) {
        auto it = m_ruleMap.constFind(id);
        if (it != m_ruleMap.constEnd())
            result.append(it.value());
    }
    return result;
}

bool AlarmManager::isDebounced(const AlarmRule &rule, const QDateTime &now) const
{
    if (rule.debounceMs <= 0)
        return false;
    const QDateTime last = m_lastTriggerTime.value(rule.id);
    if (!last.isValid())
        return false;
    // 基准是"上次真正发出事件的时间"：被去抖抑制的事件不会重置窗口
    return last.msecsTo(now) < rule.debounceMs;
}

void AlarmManager::emitTriggered(const AlarmRule &rule, double value, const QDateTime &now)
{
    m_activeStates[rule.id] = true;
    m_lastTriggerTime[rule.id] = now;
    m_pendingNotifications.remove(rule.id);

    AlarmEvent event;
    event.id = m_nextEventId++;
    event.timestamp = now;
    event.ruleId = rule.id;
    event.ruleName = rule.name;
    event.severity = rule.severity;
    event.message = rule.message.isEmpty()
        ? QString("Alarm: %1, Value: %2").arg(rule.name).arg(value)
        : rule.message;
    event.value = value;
    event.acknowledged = false;

    appendHistory(event);
    emit alarmTriggered(event);
}

void AlarmManager::appendHistory(const AlarmEvent &event)
{
    m_history.enqueue(event);

    // P2-67：历史条数上限。QQueue 的 dequeue() 是 O(1)，
    // 稳态下每条记录只搬移常数个元素，不会出现 O(n²) 的整体前移。
    const int limit = qMax(1, m_maxHistory);
    while (m_history.size() > limit)
        m_history.dequeue();
}

int AlarmManager::maxHistory() const
{
    return m_maxHistory;
}

void AlarmManager::setMaxHistory(int maxCount)
{
    m_maxHistory = (maxCount <= 0) ? defaultMaxHistory() : maxCount;
    const int limit = qMax(1, m_maxHistory);
    while (m_history.size() > limit)
        m_history.dequeue();
}

void AlarmManager::checkValue(int serverAddress, int registerType, int address, double value)
{
    // P2-71：先查地址索引，避免每个寄存器值都线性扫描全部规则
    auto serverIt = m_addressIndex.constFind(serverAddress);
    if (serverIt == m_addressIndex.constEnd())
        return;
    auto keyIt = serverIt->constFind(addressKey(registerType, address));
    if (keyIt == serverIt->constEnd())
        return;

    // 拷贝一份 id 列表：下面的 emit 可能触发槽函数增删规则，直接遍历会失效
    const QVector<int> candidateIds = keyIt.value();

    for (int ruleId : candidateIds) {
        auto ruleIt = m_ruleMap.constFind(ruleId);
        if (ruleIt == m_ruleMap.constEnd())
            continue;
        // 拷贝规则：emit 之后规则可能已被槽函数修改/删除
        const AlarmRule rule = ruleIt.value();
        if (!rule.enabled)
            continue;

        const bool triggered = evaluateCondition(rule, value);
        const bool wasActive = m_activeStates.value(rule.id, false);
        const QDateTime now = QDateTime::currentDateTime();

        if (triggered) {
            if (!wasActive) {
                // 上升沿：激活状态必须跟随真实条件（P1-11 / R-ALARM-2）
                m_activeStates[rule.id] = true;
                if (isDebounced(rule, now)) {
                    // 去抖窗口内：只抑制"事件"，不抑制"状态"，
                    // 并记下待补发标记（用户尚未被告知过这次报警）
                    m_pendingNotifications.insert(rule.id);
                } else {
                    emitTriggered(rule, value, now);
                }
            } else if (m_pendingNotifications.contains(rule.id)) {
                // 状态已是 active，但上一次的事件被去抖抑制（用户从未被告知）。
                // 去抖窗口过期后补发一次事件，避免"用户永远收不到该报警"。
                if (!isDebounced(rule, now))
                    emitTriggered(rule, value, now);
            }
        } else if (wasActive) {
            m_activeStates[rule.id] = false;
            m_pendingNotifications.remove(rule.id);
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
    if (maxCount <= 0)
        return QVector<AlarmEvent>();

    QVector<AlarmEvent> all;
    all.reserve(m_history.size());
    for (const auto &event : m_history)
        all.append(event);

    if (all.size() <= maxCount)
        return all;
    return all.mid(all.size() - maxCount);
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
    // P1-11 / R-ALARM-1：NaN 一律不参与判定。
    // qFuzzyCompare(NaN, x) 恒为 false → NotEqual 恒成立（NaN 必然误报）；
    // 浮点→整型转换 NaN 是 UB（[conv.fpint]），实际会得到 0。
    if (std::isnan(value))
        return false;

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
    case AlarmCondition::BitClear: {
        // A3：位号必须落在 0~31，越界直接判"不触发"，避免 1 << 越界为 UB
        if (!std::isfinite(rule.threshold1))
            return false;
        const int bit = static_cast<int>(rule.threshold1);
        if (bit < 0 || bit > 31)
            return false;
        // 非有限值（±Inf）无法安全转换为整数
        if (!std::isfinite(value))
            return false;
        // 只有落在 [0, 4294967295] 的值才能安全转换为 quint32，
        // 超出范围时浮点→整型转换是 UB（C++ [conv.fpint]）
        if (value < 0.0 || value > 4294967295.0)
            return false;
        const quint32 bitMask = 1u << bit;
        const quint32 raw = static_cast<quint32>(static_cast<qlonglong>(value));
        return rule.condition == AlarmCondition::BitSet
                   ? (raw & bitMask) != 0
                   : (raw & bitMask) == 0;
    }
    }
    return false;
}
