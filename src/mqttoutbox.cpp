#include "mqttoutbox.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

MqttOutbox::MqttOutbox(const QString &dbPath)
{
    m_connectionName = QStringLiteral("fieldlink_mqtt_outbox_") + QUuid::createUuid().toString();
    if (!dbPath.isEmpty())
        open(dbPath);
}

MqttOutbox::~MqttOutbox()
{
    close();
}

bool MqttOutbox::open(const QString &dbPath)
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

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));

    if (!ensureSchema()) {
        close();
        return false;
    }
    m_schemaReady = true;
    clearError();
    return true;
}

void MqttOutbox::close()
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

bool MqttOutbox::isOpen() const
{
    return m_db.isValid() && m_db.isOpen() && m_schemaReady;
}

QString MqttOutbox::databasePath() const
{
    return m_dbPath;
}

QString MqttOutbox::lastError() const
{
    return m_lastError;
}

void MqttOutbox::setError(const QString &context, const QString &detail)
{
    m_lastError = QStringLiteral("%1: %2").arg(context, detail);
    qWarning() << "MqttOutbox:" << m_lastError;
}

void MqttOutbox::clearError()
{
    m_lastError.clear();
}

bool MqttOutbox::ensureSchema()
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS mqtt_outbox ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " topic TEXT NOT NULL,"
            " payload BLOB NOT NULL,"
            " retain INTEGER NOT NULL DEFAULT 0,"
            " qos INTEGER NOT NULL DEFAULT 0,"
            " priority INTEGER NOT NULL DEFAULT 0,"
            " created_ms INTEGER NOT NULL DEFAULT 0"
            ")"))) {
        setError(QStringLiteral("schema.mqtt_outbox"), query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_mqtt_outbox_order "
            "ON mqtt_outbox(priority DESC, id ASC)"))) {
        setError(QStringLiteral("schema.idx_mqtt_outbox_order"), query.lastError().text());
        return false;
    }
    return true;
}

void MqttOutbox::setLimits(int maxMessages, qint64 maxBytes)
{
    m_maxMessages = qMax(1, maxMessages);
    m_maxBytes = qMax<qint64>(1024, maxBytes);
    if (isOpen())
        trimToLimits();
}

bool MqttOutbox::enqueue(const QString &topic, const QByteArray &payload, bool retain,
                         int qos, int priority, qint64 *outId)
{
    clearError();
    if (outId)
        *outId = 0;
    if (!isOpen()) {
        setError(QStringLiteral("enqueue"), QStringLiteral("database is not open"));
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO mqtt_outbox (topic, payload, retain, qos, priority, created_ms) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(topic);
    query.addBindValue(payload);
    query.addBindValue(retain ? 1 : 0);
    query.addBindValue(qos >= 1 ? 1 : 0);
    query.addBindValue(qBound(0, priority, 2));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        setError(QStringLiteral("enqueue"), query.lastError().text());
        return false;
    }

    const qint64 id = query.lastInsertId().toLongLong();
    if (outId)
        *outId = id;

    if (!trimToLimits())
        return false;

    // 容量策略可能把刚入队的低优先级消息淘汰掉。
    QSqlQuery exists(m_db);
    exists.prepare(QStringLiteral("SELECT 1 FROM mqtt_outbox WHERE id = ?"));
    exists.addBindValue(id);
    if (!exists.exec() || !exists.next()) {
        if (outId)
            *outId = 0;
        return false;
    }
    return true;
}

QVector<MqttOutboxMessage> MqttOutbox::peek(int maxCount) const
{
    QVector<MqttOutboxMessage> messages;
    if (maxCount <= 0 || !isOpen())
        return messages;

    QSqlDatabase db = m_db;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT id, topic, payload, retain, qos, priority, created_ms "
        "FROM mqtt_outbox ORDER BY priority DESC, id ASC LIMIT ?"));
    query.addBindValue(maxCount);
    if (!query.exec())
        return messages;

    while (query.next()) {
        MqttOutboxMessage message;
        message.id = query.value(0).toLongLong();
        message.topic = query.value(1).toString();
        message.payload = query.value(2).toByteArray();
        message.retain = query.value(3).toInt() != 0;
        message.qos = query.value(4).toInt();
        message.priority = query.value(5).toInt();
        message.createdAtMs = query.value(6).toLongLong();
        messages.append(message);
    }
    return messages;
}

bool MqttOutbox::remove(qint64 id)
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("remove"), QStringLiteral("database is not open"));
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM mqtt_outbox WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        setError(QStringLiteral("remove"), query.lastError().text());
        return false;
    }
    return true;
}

bool MqttOutbox::clear()
{
    clearError();
    if (!isOpen()) {
        setError(QStringLiteral("clear"), QStringLiteral("database is not open"));
        return false;
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("DELETE FROM mqtt_outbox"))) {
        setError(QStringLiteral("clear"), query.lastError().text());
        return false;
    }
    return true;
}

int MqttOutbox::count() const
{
    if (!isOpen())
        return 0;
    QSqlDatabase db = m_db;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM mqtt_outbox")))
        return 0;
    return query.next() ? query.value(0).toInt() : 0;
}

qint64 MqttOutbox::bytes() const
{
    if (!isOpen())
        return 0;
    QSqlDatabase db = m_db;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT COALESCE(SUM(length(CAST(topic AS BLOB)) + length(payload)), 0) "
            "FROM mqtt_outbox"))) {
        return 0;
    }
    return query.next() ? query.value(0).toLongLong() : 0;
}

bool MqttOutbox::trimToLimits()
{
    clearError();
    if (!isOpen())
        return false;

    const int currentCount = count();
    const qint64 currentBytes = bytes();
    int excessCount = currentCount - m_maxMessages;
    qint64 excessBytes = currentBytes - m_maxBytes;
    if (excessCount <= 0 && excessBytes <= 0)
        return true;

    QVector<qint64> victims;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT id, length(CAST(topic AS BLOB)) + length(payload) AS sz "
            "FROM mqtt_outbox ORDER BY priority ASC, id ASC"))) {
        setError(QStringLiteral("trim.select"), query.lastError().text());
        return false;
    }
    while (query.next() && (excessCount > 0 || excessBytes > 0)) {
        victims.append(query.value(0).toLongLong());
        --excessCount;
        excessBytes -= query.value(1).toLongLong();
    }
    if (victims.isEmpty())
        return true;

    // 分块删除，避免一次绑定过多占位符（SQLite 变量上限）。
    for (int offset = 0; offset < victims.size(); offset += 200) {
        const int end = qMin(offset + 200, victims.size());
        QStringList placeholders;
        for (int i = offset; i < end; ++i)
            placeholders.append(QStringLiteral("?"));
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral("DELETE FROM mqtt_outbox WHERE id IN (%1)")
                        .arg(placeholders.join(QLatin1Char(','))));
        for (int i = offset; i < end; ++i)
            del.addBindValue(victims.at(i));
        if (!del.exec()) {
            setError(QStringLiteral("trim.delete"), del.lastError().text());
            return false;
        }
    }
    m_droppedCount += victims.size();
    return true;
}
