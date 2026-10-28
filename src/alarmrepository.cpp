#include "alarmrepository.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <algorithm>

namespace {

AlarmCondition conditionFromInt(int value)
{
    if (value < static_cast<int>(AlarmCondition::GreaterThan)
        || value > static_cast<int>(AlarmCondition::BitClear)) {
        return AlarmCondition::GreaterThan;
    }
    return static_cast<AlarmCondition>(value);
}

AlarmSeverity severityFromInt(int value)
{
    if (value < static_cast<int>(AlarmSeverity::Info)
        || value > static_cast<int>(AlarmSeverity::Critical)) {
        return AlarmSeverity::Warning;
    }
    return static_cast<AlarmSeverity>(value);
}

QString dateTimeText(const QDateTime &value)
{
    return value.isValid() ? value.toString(Qt::ISODateWithMs) : QString();
}

qint64 dateTimeMs(const QDateTime &value)
{
    return value.isValid() ? value.toMSecsSinceEpoch() : 0;
}

QDateTime dateTimeFromColumns(const QString &text, qint64 ms)
{
    if (ms > 0)
        return QDateTime::fromMSecsSinceEpoch(ms);
    if (!text.isEmpty())
        return QDateTime::fromString(text, Qt::ISODateWithMs);
    return QDateTime();
}

} // namespace

AlarmRepository::AlarmRepository(const QString &dbPath)
{
    m_connectionName = QStringLiteral("fieldlink_alarms_") + QUuid::createUuid().toString();
    if (!dbPath.isEmpty())
        open(dbPath);
}

AlarmRepository::~AlarmRepository()
{
    close();
}

bool AlarmRepository::open(const QString &dbPath)
{
    if (m_db.isValid() && m_db.isOpen()) {
        if (m_db.databaseName() == dbPath)
            return true;
        close();
    }

    m_dbPath = dbPath;
    clearError();
    if (dbPath.trimmed().isEmpty()) {
        setError(QStringLiteral("open"), QStringLiteral("database path is empty"));
        return false;
    }

    const QFileInfo info(dbPath);
    const QDir dir(info.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        setError(QStringLiteral("open"), QStringLiteral("cannot create directory %1").arg(dir.absolutePath()));
        return false;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        const QString reason = m_db.lastError().text().isEmpty()
            ? QStringLiteral("unknown error") : m_db.lastError().text();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        setError(QStringLiteral("open"), QStringLiteral("%1: %2").arg(dbPath, reason));
        return false;
    }

    // 独立数据库，WAL + NORMAL 足以兼顾可靠性与写入延迟。
    execPragma(QStringLiteral("PRAGMA journal_mode=WAL"));
    execPragma(QStringLiteral("PRAGMA synchronous=NORMAL"));
    execPragma(QStringLiteral("PRAGMA busy_timeout=3000"));

    if (!ensureSchema()) {
        close();
        return false;
    }
    m_schemaReady = true;
    clearError();
    return true;
}

void AlarmRepository::close()
{
    m_schemaReady = false;
    const QString connection = m_connectionName;
    if (m_db.isValid()) {
        if (m_db.isOpen())
            m_db.close();
        m_db = QSqlDatabase();
    }
    if (!connection.isEmpty() && QSqlDatabase::contains(connection))
        QSqlDatabase::removeDatabase(connection);
}

bool AlarmRepository::isOpen() const
{
    return m_db.isValid() && m_db.isOpen() && m_schemaReady;
}

QString AlarmRepository::databasePath() const
{
    return m_dbPath;
}

QString AlarmRepository::lastError() const
{
    return m_lastError;
}

void AlarmRepository::setError(const QString &context, const QString &detail)
{
    m_lastError = QStringLiteral("%1: %2").arg(context, detail);
    qWarning() << "AlarmRepository:" << m_lastError;
}

void AlarmRepository::clearError()
{
    m_lastError.clear();
}

bool AlarmRepository::execPragma(const QString &sql)
{
    QSqlQuery query(m_db);
    if (!query.exec(sql)) {
        setError(QStringLiteral("pragma"), query.lastError().text());
        return false;
    }
    return true;
}

bool AlarmRepository::ensureSchema()
{
    QSqlQuery query(m_db);
    const QString ruleSql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS alarm_rules ("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL DEFAULT '',"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " device_id INTEGER NOT NULL DEFAULT 0,"
        " point_id INTEGER NOT NULL DEFAULT 0,"
        " server_address INTEGER NOT NULL DEFAULT 1,"
        " register_type INTEGER NOT NULL DEFAULT 0,"
        " address INTEGER NOT NULL DEFAULT 0,"
        " cond INTEGER NOT NULL DEFAULT 0,"
        " threshold1 REAL NOT NULL DEFAULT 0,"
        " threshold2 REAL NOT NULL DEFAULT 0,"
        " severity INTEGER NOT NULL DEFAULT 1,"
        " message TEXT NOT NULL DEFAULT '',"
        " debounce_ms INTEGER NOT NULL DEFAULT 0,"
        " acknowledged INTEGER NOT NULL DEFAULT 0"
        ")");
    if (!query.exec(ruleSql)) {
        setError(QStringLiteral("schema.alarm_rules"), query.lastError().text());
        return false;
    }

    const QString eventSql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS alarm_events ("
        " id INTEGER PRIMARY KEY,"
        " timestamp_text TEXT NOT NULL DEFAULT '',"
        " timestamp_ms INTEGER NOT NULL DEFAULT 0,"
        " rule_id INTEGER NOT NULL DEFAULT 0,"
        " rule_name TEXT NOT NULL DEFAULT '',"
        " device_id INTEGER NOT NULL DEFAULT 0,"
        " point_id INTEGER NOT NULL DEFAULT 0,"
        " server_address INTEGER NOT NULL DEFAULT 0,"
        " register_type INTEGER NOT NULL DEFAULT 0,"
        " address INTEGER NOT NULL DEFAULT 0,"
        " cond INTEGER NOT NULL DEFAULT 0,"
        " severity INTEGER NOT NULL DEFAULT 1,"
        " alarm_value REAL NOT NULL DEFAULT 0,"
        " message TEXT NOT NULL DEFAULT '',"
        " acknowledged INTEGER NOT NULL DEFAULT 0,"
        " acknowledged_text TEXT NOT NULL DEFAULT '',"
        " acknowledged_ms INTEGER NOT NULL DEFAULT 0"
        ")");
    if (!query.exec(eventSql)) {
        setError(QStringLiteral("schema.alarm_events"), query.lastError().text());
        return false;
    }

    const QString stateSql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS alarm_states ("
        " rule_id INTEGER PRIMARY KEY,"
        " active INTEGER NOT NULL DEFAULT 0,"
        " last_trigger_ms INTEGER NOT NULL DEFAULT 0"
        ")");
    if (!query.exec(stateSql)) {
        setError(QStringLiteral("schema.alarm_states"), query.lastError().text());
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_alarm_events_time ON alarm_events(timestamp_ms)"))) {
        setError(QStringLiteral("schema.idx_alarm_events_time"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_alarm_events_rule ON alarm_events(rule_id)"))) {
        setError(QStringLiteral("schema.idx_alarm_events_rule"), query.lastError().text());
        return false;
    }
    return true;
}

bool AlarmRepository::saveRule(const AlarmRule &rule)
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("saveRule"), QStringLiteral("database is not open"));
        return false;
    }
    if (rule.id <= 0) {
        setError(QStringLiteral("saveRule"), QStringLiteral("invalid rule id %1").arg(rule.id));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO alarm_rules "
        "(id, name, enabled, device_id, point_id, server_address, register_type, address,"
        " cond, threshold1, threshold2, severity, message, debounce_ms, acknowledged) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(rule.id);
    query.addBindValue(rule.name);
    query.addBindValue(rule.enabled ? 1 : 0);
    query.addBindValue(rule.deviceId);
    query.addBindValue(rule.pointId);
    query.addBindValue(rule.serverAddress);
    query.addBindValue(rule.registerType);
    query.addBindValue(rule.address);
    query.addBindValue(static_cast<int>(rule.condition));
    query.addBindValue(rule.threshold1);
    query.addBindValue(rule.threshold2);
    query.addBindValue(static_cast<int>(rule.severity));
    query.addBindValue(rule.message);
    query.addBindValue(rule.debounceMs);
    query.addBindValue(rule.acknowledged ? 1 : 0);
    if (!query.exec()) {
        setError(QStringLiteral("saveRule"), query.lastError().text());
        return false;
    }
    return true;
}

bool AlarmRepository::deleteRule(int ruleId)
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("deleteRule"), QStringLiteral("database is not open"));
        return false;
    }

    if (!m_db.transaction()) {
        setError(QStringLiteral("deleteRule.transaction"), m_db.lastError().text());
        return false;
    }

    QSqlQuery query(m_db);
    bool ok = true;
    query.prepare(QStringLiteral("DELETE FROM alarm_events WHERE rule_id = ?"));
    query.addBindValue(ruleId);
    ok = query.exec() && ok;

    query.prepare(QStringLiteral("DELETE FROM alarm_states WHERE rule_id = ?"));
    query.addBindValue(ruleId);
    ok = query.exec() && ok;

    query.prepare(QStringLiteral("DELETE FROM alarm_rules WHERE id = ?"));
    query.addBindValue(ruleId);
    ok = query.exec() && ok;

    if (!ok) {
        setError(QStringLiteral("deleteRule"), query.lastError().text());
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        setError(QStringLiteral("deleteRule.commit"), m_db.lastError().text());
        m_db.rollback();
        return false;
    }
    return true;
}

QVector<AlarmRule> AlarmRepository::loadRules()
{
    clearError();
    QVector<AlarmRule> rules;
    if (!isOpen()) {
        setError(QStringLiteral("loadRules"), QStringLiteral("database is not open"));
        return rules;
    }

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT * FROM alarm_rules ORDER BY id ASC"))) {
        setError(QStringLiteral("loadRules"), query.lastError().text());
        return rules;
    }
    while (query.next()) {
        AlarmRule rule;
        rule.id = query.value(QStringLiteral("id")).toInt();
        rule.name = query.value(QStringLiteral("name")).toString();
        rule.enabled = query.value(QStringLiteral("enabled")).toInt() != 0;
        rule.deviceId = query.value(QStringLiteral("device_id")).toInt();
        rule.pointId = query.value(QStringLiteral("point_id")).toInt();
        rule.serverAddress = query.value(QStringLiteral("server_address")).toInt();
        rule.registerType = query.value(QStringLiteral("register_type")).toInt();
        rule.address = query.value(QStringLiteral("address")).toInt();
        rule.condition = conditionFromInt(query.value(QStringLiteral("cond")).toInt());
        rule.threshold1 = query.value(QStringLiteral("threshold1")).toDouble();
        rule.threshold2 = query.value(QStringLiteral("threshold2")).toDouble();
        rule.severity = severityFromInt(query.value(QStringLiteral("severity")).toInt());
        rule.message = query.value(QStringLiteral("message")).toString();
        rule.debounceMs = query.value(QStringLiteral("debounce_ms")).toInt();
        rule.acknowledged = query.value(QStringLiteral("acknowledged")).toInt() != 0;
        rules.append(rule);
    }
    return rules;
}

bool AlarmRepository::saveEvent(const AlarmEvent &event)
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("saveEvent"), QStringLiteral("database is not open"));
        return false;
    }
    if (event.id <= 0) {
        setError(QStringLiteral("saveEvent"), QStringLiteral("invalid event id %1").arg(event.id));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO alarm_events "
        "(id, timestamp_text, timestamp_ms, rule_id, rule_name, device_id, point_id,"
        " server_address, register_type, address, cond, severity, alarm_value, message,"
        " acknowledged, acknowledged_text, acknowledged_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event.id);
    query.addBindValue(dateTimeText(event.timestamp));
    query.addBindValue(dateTimeMs(event.timestamp));
    query.addBindValue(event.ruleId);
    query.addBindValue(event.ruleName);
    query.addBindValue(event.deviceId);
    query.addBindValue(event.pointId);
    query.addBindValue(event.serverAddress);
    query.addBindValue(event.registerType);
    query.addBindValue(event.address);
    query.addBindValue(static_cast<int>(event.condition));
    query.addBindValue(static_cast<int>(event.severity));
    query.addBindValue(event.value);
    query.addBindValue(event.message);
    query.addBindValue(event.acknowledged ? 1 : 0);
    query.addBindValue(dateTimeText(event.acknowledgedTime));
    query.addBindValue(dateTimeMs(event.acknowledgedTime));
    if (!query.exec()) {
        setError(QStringLiteral("saveEvent"), query.lastError().text());
        return false;
    }
    return true;
}

bool AlarmRepository::updateEventAck(qint64 eventId, bool acknowledged, const QDateTime &acknowledgedTime)
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("updateEventAck"), QStringLiteral("database is not open"));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE alarm_events SET acknowledged = ?, acknowledged_text = ?, acknowledged_ms = ? "
        "WHERE id = ?"));
    query.addBindValue(acknowledged ? 1 : 0);
    query.addBindValue(dateTimeText(acknowledgedTime));
    query.addBindValue(dateTimeMs(acknowledgedTime));
    query.addBindValue(eventId);
    if (!query.exec()) {
        setError(QStringLiteral("updateEventAck"), query.lastError().text());
        return false;
    }
    return true;
}

bool AlarmRepository::acknowledgeAllEvents()
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("acknowledgeAllEvents"), QStringLiteral("database is not open"));
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE alarm_events SET acknowledged = 1, acknowledged_text = ?, acknowledged_ms = ? "
        "WHERE acknowledged = 0"));
    query.addBindValue(dateTimeText(now));
    query.addBindValue(dateTimeMs(now));
    if (!query.exec()) {
        setError(QStringLiteral("acknowledgeAllEvents"), query.lastError().text());
        return false;
    }
    return true;
}

bool AlarmRepository::deleteAllEvents()
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("deleteAllEvents"), QStringLiteral("database is not open"));
        return false;
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("DELETE FROM alarm_events"))) {
        setError(QStringLiteral("deleteAllEvents"), query.lastError().text());
        return false;
    }
    return true;
}

QVector<AlarmEvent> AlarmRepository::loadEvents(int maxCount)
{
    clearError();
    QVector<AlarmEvent> events;
    if (!isOpen()) {
        setError(QStringLiteral("loadEvents"), QStringLiteral("database is not open"));
        return events;
    }
    if (maxCount <= 0)
        return events;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM alarm_events ORDER BY id DESC LIMIT ?"));
    query.addBindValue(maxCount);
    if (!query.exec()) {
        setError(QStringLiteral("loadEvents"), query.lastError().text());
        return events;
    }
    while (query.next()) {
        AlarmEvent event;
        event.id = query.value(QStringLiteral("id")).toLongLong();
        event.timestamp = dateTimeFromColumns(
            query.value(QStringLiteral("timestamp_text")).toString(),
            query.value(QStringLiteral("timestamp_ms")).toLongLong());
        event.ruleId = query.value(QStringLiteral("rule_id")).toInt();
        event.ruleName = query.value(QStringLiteral("rule_name")).toString();
        event.deviceId = query.value(QStringLiteral("device_id")).toInt();
        event.pointId = query.value(QStringLiteral("point_id")).toInt();
        event.serverAddress = query.value(QStringLiteral("server_address")).toInt();
        event.registerType = query.value(QStringLiteral("register_type")).toInt();
        event.address = query.value(QStringLiteral("address")).toInt();
        event.condition = conditionFromInt(query.value(QStringLiteral("cond")).toInt());
        event.severity = severityFromInt(query.value(QStringLiteral("severity")).toInt());
        event.value = query.value(QStringLiteral("alarm_value")).toDouble();
        event.message = query.value(QStringLiteral("message")).toString();
        event.acknowledged = query.value(QStringLiteral("acknowledged")).toInt() != 0;
        event.acknowledgedTime = dateTimeFromColumns(
            query.value(QStringLiteral("acknowledged_text")).toString(),
            query.value(QStringLiteral("acknowledged_ms")).toLongLong());
        events.append(event);
    }
    std::reverse(events.begin(), events.end());
    return events;
}

bool AlarmRepository::saveState(int ruleId, bool active, const QDateTime &lastTriggerTime)
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("saveState"), QStringLiteral("database is not open"));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO alarm_states (rule_id, active, last_trigger_ms) VALUES (?, ?, ?)"));
    query.addBindValue(ruleId);
    query.addBindValue(active ? 1 : 0);
    query.addBindValue(dateTimeMs(lastTriggerTime));
    if (!query.exec()) {
        setError(QStringLiteral("saveState"), query.lastError().text());
        return false;
    }
    return true;
}

bool AlarmRepository::deleteState(int ruleId)
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("deleteState"), QStringLiteral("database is not open"));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM alarm_states WHERE rule_id = ?"));
    query.addBindValue(ruleId);
    if (!query.exec()) {
        setError(QStringLiteral("deleteState"), query.lastError().text());
        return false;
    }
    return true;
}

QVector<AlarmRuleState> AlarmRepository::loadStates()
{
    clearError();
    QVector<AlarmRuleState> states;
    if (!isOpen()) {
        setError(QStringLiteral("loadStates"), QStringLiteral("database is not open"));
        return states;
    }

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT * FROM alarm_states ORDER BY rule_id ASC"))) {
        setError(QStringLiteral("loadStates"), query.lastError().text());
        return states;
    }
    while (query.next()) {
        AlarmRuleState state;
        state.ruleId = query.value(QStringLiteral("rule_id")).toInt();
        state.active = query.value(QStringLiteral("active")).toInt() != 0;
        const qint64 ms = query.value(QStringLiteral("last_trigger_ms")).toLongLong();
        if (ms > 0)
            state.lastTriggerTime = QDateTime::fromMSecsSinceEpoch(ms);
        states.append(state);
    }
    return states;
}

int AlarmRepository::nextRuleId()
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("nextRuleId"), QStringLiteral("database is not open"));
        return 1;
    }

    int maxId = 0;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT MAX(id) FROM alarm_rules"))) {
        setError(QStringLiteral("nextRuleId.rule"), query.lastError().text());
        return 1;
    }
    if (query.next())
        maxId = query.value(0).toInt();

    // 已删除规则的事件仍可能引用更大的 rule_id，避免新规则复用旧 ID 造成历史串号。
    if (!query.exec(QStringLiteral("SELECT MAX(rule_id) FROM alarm_events"))) {
        setError(QStringLiteral("nextRuleId.event"), query.lastError().text());
        return maxId + 1;
    }
    if (query.next())
        maxId = qMax(maxId, query.value(0).toInt());

    return maxId + 1;
}

qint64 AlarmRepository::nextEventId()
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("nextEventId"), QStringLiteral("database is not open"));
        return 1;
    }

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT MAX(id) FROM alarm_events"))) {
        setError(QStringLiteral("nextEventId"), query.lastError().text());
        return 1;
    }
    const qint64 maxId = query.next() ? query.value(0).toLongLong() : 0;
    return maxId + 1;
}

int AlarmRepository::ruleCount()
{
    clearError();
    if (!isOpen())
        return 0;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM alarm_rules"))) {
        setError(QStringLiteral("ruleCount"), query.lastError().text());
        return 0;
    }
    return query.next() ? query.value(0).toInt() : 0;
}

int AlarmRepository::eventCount()
{
    clearError();
    if (!isOpen())
        return 0;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM alarm_events"))) {
        setError(QStringLiteral("eventCount"), query.lastError().text());
        return 0;
    }
    return query.next() ? query.value(0).toInt() : 0;
}
