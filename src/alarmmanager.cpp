#include "alarmmanager.h"
#include "alarmrepository.h"

#include <QMap>
#include <QHash>
#include <QSet>
#include <QDebug>
#include <cmath>

AlarmManager::AlarmManager(QObject *parent)
    : QObject(parent)
    , m_nextRuleId(1)
    , m_nextEventId(1)
{
}

void AlarmManager::setRepository(AlarmRepository *repository)
{
    if (m_repository == repository)
        return;
    m_repository = repository;
    m_storageErrorReported = false;

    // 启动阶段（内存为空）才自动恢复，避免在已有运行态上被磁盘快照覆盖。
    if (m_repository && m_repository->isOpen()
        && m_ruleOrder.isEmpty() && m_history.isEmpty()) {
        reloadPersistedState();
    }
}

bool AlarmManager::reloadPersistedState()
{
    if (!m_repository || !m_repository->isOpen())
        return false;

    const QVector<AlarmRule> rules = m_repository->loadRules();
    if (!m_repository->lastError().isEmpty()) {
        reportStorageError(m_repository->lastError());
        return false;
    }
    const QVector<AlarmEvent> events = m_repository->loadEvents(qMax(1, m_maxHistory));
    if (!m_repository->lastError().isEmpty()) {
        reportStorageError(m_repository->lastError());
        return false;
    }
    const QVector<AlarmRuleState> states = m_repository->loadStates();
    if (!m_repository->lastError().isEmpty()) {
        reportStorageError(m_repository->lastError());
        return false;
    }

    m_ruleOrder.clear();
    m_ruleMap.clear();
    m_addressIndex.clear();
    m_pointIndex.clear();
    m_history.clear();
    m_activeStates.clear();
    m_lastTriggerTime.clear();
    m_pendingNotifications.clear();

    int maxRuleId = 0;
    for (const AlarmRule &rule : rules) {
        if (rule.id <= 0)
            continue;
        m_ruleMap.insert(rule.id, rule);
        m_ruleOrder.append(rule.id);
        m_activeStates.insert(rule.id, false);
        maxRuleId = qMax(maxRuleId, rule.id);
    }

    for (const AlarmRuleState &state : states) {
        if (state.ruleId <= 0 || !m_ruleMap.contains(state.ruleId))
            continue;
        m_activeStates[state.ruleId] = state.active;
        if (state.lastTriggerTime.isValid())
            m_lastTriggerTime[state.ruleId] = state.lastTriggerTime;
    }

    qint64 maxEventId = 0;
    for (const AlarmEvent &event : events) {
        if (event.id <= 0)
            continue;
        m_history.enqueue(event);
        maxEventId = qMax(maxEventId, event.id);
    }

    m_nextRuleId = qMax(1, maxRuleId + 1);
    m_nextEventId = qMax<qint64>(1, maxEventId + 1);
    rebuildIndex();
    m_storageErrorReported = false;
    return true;
}

void AlarmManager::reportStorageError(const QString &message)
{
    if (m_storageErrorReported)
        return;
    m_storageErrorReported = true;
    qWarning() << "AlarmManager persistence:" << message;
    emit storageError(message);
}

void AlarmManager::persistRule(const AlarmRule &rule)
{
    if (!m_repository || !m_repository->isOpen())
        return;
    if (m_repository->saveRule(rule))
        m_storageErrorReported = false;
    else
        reportStorageError(m_repository->lastError());
}

void AlarmManager::persistEvent(const AlarmEvent &event)
{
    if (!m_repository || !m_repository->isOpen())
        return;
    if (m_repository->saveEvent(event))
        m_storageErrorReported = false;
    else
        reportStorageError(m_repository->lastError());
}

void AlarmManager::persistState(int ruleId, bool active, const QDateTime &lastTriggerTime)
{
    if (!m_repository || !m_repository->isOpen())
        return;
    if (m_repository->saveState(ruleId, active, lastTriggerTime))
        m_storageErrorReported = false;
    else
        reportStorageError(m_repository->lastError());
}

quint64 AlarmManager::addressKey(int registerType, int address)
{
    // 高 32 位放寄存器类型，低 32 位放寄存器地址，避免不同类型/地址互相碰撞
    return (quint64(quint32(registerType)) << 32) | quint64(quint32(address));
}

quint64 AlarmManager::pointKey(int deviceId, int pointId)
{
    return (quint64(quint32(deviceId)) << 32) | quint64(quint32(pointId));
}

void AlarmManager::rebuildIndex()
{
    m_addressIndex.clear();
    m_pointIndex.clear();
    for (int id : m_ruleOrder) {
        auto it = m_ruleMap.constFind(id);
        if (it == m_ruleMap.constEnd())
            continue;
        const AlarmRule &r = it.value();
        if (r.pointId > 0)
            m_pointIndex[pointKey(r.deviceId, r.pointId)].append(id);
        else
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
    persistRule(r);
    // 清除可能残留的同 ID 状态（理论上 ID 不复用，这里做防御）。
    persistState(r.id, false, QDateTime());
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
    if (m_repository && m_repository->isOpen()) {
        if (m_repository->deleteRule(ruleId))
            m_storageErrorReported = false;
        else
            reportStorageError(m_repository->lastError());
    }
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
    persistRule(it.value());
    persistState(rule.id, false, QDateTime());
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
    persistRule(it.value());
    persistState(ruleId, m_activeStates.value(ruleId, false), m_lastTriggerTime.value(ruleId));
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
    event.deviceId = rule.deviceId;
    event.pointId = rule.pointId;
    event.serverAddress = rule.serverAddress;
    event.registerType = rule.registerType;
    event.address = rule.address;
    event.condition = rule.condition;

    appendHistory(event);
    persistState(rule.id, true, now);
    emit alarmTriggered(event);
}

void AlarmManager::appendHistory(const AlarmEvent &event)
{
    m_history.enqueue(event);
    persistEvent(event);

    // P2-67：历史条数上限。QQueue 的 dequeue() 是 O(1)，
    // 稳态下每条记录只搬移常数个元素，不会出现 O(n²) 的整体前移。
    // 注意：只裁剪内存中的展示窗口，数据库保留完整事件用于审计。
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

    evaluateCandidates(keyIt.value(), value);
}

void AlarmManager::checkPointValue(int deviceId, int pointId, double value, int quality)
{
    // DataQuality: Good=0, Bad=1, Timeout=2, OutOfRange=3, Unknown=4
    // 通信异常期间不评估过程报警，否则一次断线会让所有点位同时"超限"。
    if (quality == 1 || quality == 2 || quality == 4)
        return;

    auto it = m_pointIndex.constFind(pointKey(deviceId, pointId));
    if (it == m_pointIndex.constEnd())
        return;

    evaluateCandidates(it.value(), value);
}

void AlarmManager::evaluateCandidates(const QVector<int> &candidateIds, double value)
{
    // 拷贝一份 id 列表：下面的 emit 可能触发槽函数增删规则，直接遍历会失效
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
            persistState(rule.id, false, m_lastTriggerTime.value(rule.id));
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
            if (m_repository && m_repository->isOpen()) {
                if (m_repository->updateEventAck(event.id, true, event.acknowledgedTime))
                    m_storageErrorReported = false;
                else
                    reportStorageError(m_repository->lastError());
            }
            emit alarmAcknowledged(eventId);
            break;
        }
    }
}

void AlarmManager::acknowledgeAll()
{
    const QDateTime now = QDateTime::currentDateTime();
    bool changed = false;
    for (auto &event : m_history) {
        if (!event.acknowledged) {
            event.acknowledged = true;
            event.acknowledgedTime = now;
            changed = true;
            emit alarmAcknowledged(event.id);
        }
    }
    if (changed && m_repository && m_repository->isOpen()) {
        if (m_repository->acknowledgeAllEvents())
            m_storageErrorReported = false;
        else
            reportStorageError(m_repository->lastError());
    }
}

void AlarmManager::clearHistory()
{
    m_history.clear();
    if (m_repository && m_repository->isOpen()) {
        if (m_repository->deleteAllEvents())
            m_storageErrorReported = false;
        else
            reportStorageError(m_repository->lastError());
    }
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
