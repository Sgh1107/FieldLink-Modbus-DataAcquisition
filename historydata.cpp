#include "historydata.h"
#include <QSqlError>
#include <QSqlRecord>
#include <QUuid>

HistoryData::HistoryData(QObject *parent)
    : QObject(parent)
{
    m_connectionName = "modbus_history_" + QUuid::createUuid().toString();
}

HistoryData::~HistoryData()
{
    closeDatabase();
}

bool HistoryData::openDatabase(const QString &dbPath)
{
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(dbPath);
    if (!m_db.open())
        return false;
    createTables();
    return true;
}

void HistoryData::closeDatabase()
{
    const QString connection = m_connectionName;
    if (m_db.isValid()) {
        if (m_db.isOpen())
            m_db.close();
        m_db = QSqlDatabase();
    }
    if (QSqlDatabase::contains(connection))
        QSqlDatabase::removeDatabase(connection);
}

bool HistoryData::isOpen() const
{
    return m_db.isOpen();
}

void HistoryData::createTables()
{
    QSqlQuery query(m_db);
    query.exec(
        "CREATE TABLE IF NOT EXISTS history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp TEXT NOT NULL,"
        "  server_address INTEGER,"
        "  register_type INTEGER,"
        "  start_address INTEGER,"
        "  count INTEGER,"
        "  values_data TEXT"
        ")"
    );
    query.exec("CREATE INDEX IF NOT EXISTS idx_timestamp ON history(timestamp)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_server ON history(server_address)");
}

void HistoryData::addRecord(int serverAddress, QModbusDataUnit::RegisterType regType,
                            int startAddress, const QVector<quint16> &values)
{
    if (!m_db.isOpen())
        return;

    static int cleanupCounter = 0;
    if (++cleanupCounter >= 1000) {
        cleanupCounter = 0;
        clearOlderThan(QDateTime::currentDateTime().addDays(-30));
    }

    QSqlQuery query(m_db);
    query.prepare(
        "INSERT INTO history (timestamp, server_address, register_type, start_address, count, values_data) "
        "VALUES (?, ?, ?, ?, ?, ?)"
    );
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    query.addBindValue(serverAddress);
    query.addBindValue(static_cast<int>(regType));
    query.addBindValue(startAddress);
    query.addBindValue(values.size());

    QStringList valStrs;
    for (quint16 v : values)
        valStrs.append(QString::number(v));
    query.addBindValue(valStrs.join(","));
    query.exec();
}

QVector<HistoryRecord> HistoryData::query(const QDateTime &from, const QDateTime &to,
                                           int serverAddress, int registerType,
                                           int startAddress) const
{
    QString sql = "SELECT * FROM history WHERE timestamp >= ? AND timestamp <= ?";
    if (serverAddress >= 0) sql += " AND server_address = " + QString::number(serverAddress);
    if (registerType >= 0) sql += " AND register_type = " + QString::number(registerType);
    if (startAddress >= 0) sql += " AND start_address = " + QString::number(startAddress);
    sql += " ORDER BY timestamp DESC";

    QSqlQuery q(m_db);
    q.prepare(sql);
    q.addBindValue(from.toString(Qt::ISODateWithMs));
    q.addBindValue(to.toString(Qt::ISODateWithMs));
    q.exec();

    QVector<HistoryRecord> results;
    while (q.next()) {
        HistoryRecord rec;
        rec.id = q.value("id").toLongLong();
        rec.timestamp = QDateTime::fromString(q.value("timestamp").toString(), Qt::ISODateWithMs);
        rec.serverAddress = q.value("server_address").toInt();
        rec.registerType = static_cast<QModbusDataUnit::RegisterType>(q.value("register_type").toInt());
        rec.startAddress = q.value("start_address").toInt();
        rec.count = q.value("count").toInt();

        QString valData = q.value("values_data").toString();
        for (const QString &s : valData.split(",", Qt::SkipEmptyParts))
            rec.values.append(s.toUShort());
        results.append(rec);
    }
    return results;
}

QVector<HistoryRecord> HistoryData::lastRecords(int count) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM history ORDER BY id DESC LIMIT ?");
    q.addBindValue(count);
    q.exec();

    QVector<HistoryRecord> results;
    while (q.next()) {
        HistoryRecord rec;
        rec.id = q.value("id").toLongLong();
        rec.timestamp = QDateTime::fromString(q.value("timestamp").toString(), Qt::ISODateWithMs);
        rec.serverAddress = q.value("server_address").toInt();
        rec.registerType = static_cast<QModbusDataUnit::RegisterType>(q.value("register_type").toInt());
        rec.startAddress = q.value("start_address").toInt();
        rec.count = q.value("count").toInt();

        QString valData = q.value("values_data").toString();
        for (const QString &s : valData.split(",", Qt::SkipEmptyParts))
            rec.values.append(s.toUShort());
        results.append(rec);
    }
    return results;
}

int HistoryData::totalRecords() const
{
    QSqlQuery q(m_db);
    q.exec("SELECT COUNT(*) FROM history");
    if (q.next()) return q.value(0).toInt();
    return 0;
}

void HistoryData::clearOlderThan(const QDateTime &before)
{
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM history WHERE timestamp < ?");
    q.addBindValue(before.toString(Qt::ISODateWithMs));
    q.exec();
}

void HistoryData::clearAll()
{
    QSqlQuery q(m_db);
    q.exec("DELETE FROM history");
}
