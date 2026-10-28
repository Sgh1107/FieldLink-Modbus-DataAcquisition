#include "historydata.h"
#include <QSqlError>
#include <QSqlRecord>
#include <QUuid>
#include <QVariantList>
#include <QDebug>

namespace {
// P2-14：每插入多少条触发一次保留期清理
const int kCleanupInterval = 1000;
} // namespace

HistoryData::HistoryData(QObject *parent)
    : QObject(parent)
{
    m_connectionName = "modbus_history_" + QUuid::createUuid().toString();
}

HistoryData::~HistoryData()
{
    closeDatabase();
}

void HistoryData::setLastError(const QString &message) const
{
    m_lastError = message;
}

QString HistoryData::lastError() const
{
    return m_lastError;
}

int HistoryData::retentionDays() const
{
    return m_retentionDays;
}

void HistoryData::setRetentionDays(int days)
{
    // 0 或负值 = 不自动清理
    m_retentionDays = (days < 0) ? 0 : days;
}

bool HistoryData::openDatabase(const QString &dbPath)
{
    // 已打开：同一路径直接返回成功；不同路径则先关闭再切换
    if (m_db.isValid() && m_db.isOpen()) {
        if (m_db.databaseName() == dbPath)
            return true;
        closeDatabase();
    }

    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        // P2-8/P2-17：打开失败时清理连接名，避免后续 addDatabase 重复连接警告
        const QString reason = m_db.lastError().text().isEmpty()
            ? QStringLiteral("未知错误")
            : m_db.lastError().text();
        setLastError(QStringLiteral("打开历史数据库失败(%1): %2").arg(dbPath, reason));
        qWarning() << "HistoryData::openDatabase:" << m_lastError;
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }

    setLastError(QString());
    createTables();
    applyPragmas();
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
    // P2-08：组合索引——按 从站+类型+起始地址+时间 的组合查询是历史页的主查询路径
    query.exec("CREATE INDEX IF NOT EXISTS idx_hist_lookup "
               "ON history(server_address, register_type, start_address, timestamp)");

    // 工程值归档：按点位存储 scale/offset/字节序换算后的值，并保留质量码。
    // 原始寄存器表保留用于报文级追溯；点位表用于报表/趋势/聚合查询。
    query.exec(
        "CREATE TABLE IF NOT EXISTS point_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp TEXT NOT NULL,"
        "  device_id INTEGER NOT NULL DEFAULT 0,"
        "  point_id INTEGER NOT NULL,"
        "  point_name TEXT,"
        "  server_address INTEGER,"
        "  value REAL,"
        "  quality INTEGER"
        ")"
    );
    query.exec("CREATE INDEX IF NOT EXISTS idx_point_hist_lookup "
               "ON point_history(point_id, timestamp)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_point_hist_device "
               "ON point_history(device_id, timestamp)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_point_hist_time ON point_history(timestamp)");
}

void HistoryData::applyPragmas()
{
    // P2-11：WAL + synchronous=NORMAL，避免每条 INSERT 都 fsync 一次
    QSqlQuery query(m_db);
    query.exec("PRAGMA journal_mode=WAL");
    query.exec("PRAGMA synchronous=NORMAL");
}

void HistoryData::addRecord(int serverAddress, QModbusDataUnit::RegisterType regType,
                            int startAddress, const QVector<quint16> &values)
{
    HistoryBatchEntry entry;
    entry.serverAddress = serverAddress;
    entry.registerType = regType;
    entry.startAddress = startAddress;
    entry.values = values;

    QVector<HistoryBatchEntry> batch;
    batch.append(entry);
    addRecords(batch);
}

bool HistoryData::writeEntry(QSqlQuery &query, const HistoryBatchEntry &entry,
                              const QDateTime &timestamp)
{
    query.addBindValue(timestamp.toString(Qt::ISODateWithMs));
    query.addBindValue(entry.serverAddress);
    query.addBindValue(static_cast<int>(entry.registerType));
    query.addBindValue(entry.startAddress);
    query.addBindValue(entry.values.size());

    QStringList valStrs;
    for (quint16 v : entry.values)
        valStrs.append(QString::number(v));
    query.addBindValue(valStrs.join(","));

    return query.exec();
}

bool HistoryData::addRecords(const QVector<HistoryBatchEntry> &records)
{
    if (!m_db.isOpen()) {
        // P2-12：写失败不能静默丢数据，必须留下可查的错误描述
        setLastError(QStringLiteral("历史数据库未打开，记录未写入"));
        qWarning() << "HistoryData::addRecords: 数据库未打开，丢弃" << records.size() << "条记录";
        return false;
    }
    if (records.isEmpty()) {
        setLastError(QString());
        return true;
    }

    QSqlQuery query(m_db);
    if (!query.prepare(
            "INSERT INTO history (timestamp, server_address, register_type, start_address, count, values_data) "
            "VALUES (?, ?, ?, ?, ?, ?)")) {
        setLastError(QStringLiteral("准备 INSERT 失败: %1").arg(query.lastError().text()));
        qWarning() << "HistoryData::addRecords:" << m_lastError;
        return false;
    }

    // 一个事务提交整批，避免逐条 autocommit 的写放大
    const bool ownTransaction = m_db.transaction();
    for (const HistoryBatchEntry &entry : records) {
        const QDateTime ts = entry.timestamp.isValid() ? entry.timestamp : QDateTime::currentDateTime();
        if (!writeEntry(query, entry, ts)) {
            setLastError(QStringLiteral("写入历史记录失败(从站%1/地址%2): %3")
                             .arg(entry.serverAddress).arg(entry.startAddress)
                             .arg(query.lastError().text()));
            qWarning() << "HistoryData::addRecords:" << m_lastError;
            if (ownTransaction)
                m_db.rollback();
            return false;
        }
    }
    if (ownTransaction && !m_db.commit()) {
        setLastError(QStringLiteral("提交历史记录事务失败: %1").arg(m_db.lastError().text()));
        qWarning() << "HistoryData::addRecords:" << m_lastError;
        return false;
    }

    setLastError(QString());

    // 保留期清理放在写事务之外，避免大 DELETE 拖慢采集写入
    maybeAutoCleanup();
    return true;
}

bool HistoryData::writePointEntry(QSqlQuery &query, const PointHistoryRecord &entry,
                                  const QDateTime &timestamp)
{
    query.addBindValue(timestamp.toString(Qt::ISODateWithMs));
    query.addBindValue(entry.deviceId);
    query.addBindValue(entry.pointId);
    query.addBindValue(entry.pointName);
    query.addBindValue(entry.serverAddress);
    query.addBindValue(entry.value);
    query.addBindValue(entry.quality);
    return query.exec();
}

bool HistoryData::addPointRecord(const PointHistoryRecord &record)
{
    return addPointRecords(QVector<PointHistoryRecord>{record});
}

bool HistoryData::addPointRecords(const QVector<PointHistoryRecord> &records)
{
    if (!m_db.isOpen()) {
        setLastError(QStringLiteral("历史数据库未打开，工程值记录未写入"));
        return false;
    }
    if (records.isEmpty()) {
        setLastError(QString());
        return true;
    }

    QSqlQuery query(m_db);
    if (!query.prepare(
            "INSERT INTO point_history (timestamp, device_id, point_id, point_name, "
            "server_address, value, quality) VALUES (?, ?, ?, ?, ?, ?, ?)")) {
        setLastError(QStringLiteral("准备点位归档 INSERT 失败: %1").arg(query.lastError().text()));
        qWarning() << "HistoryData::addPointRecords:" << m_lastError;
        return false;
    }

    const bool ownTransaction = m_db.transaction();
    for (const PointHistoryRecord &entry : records) {
        const QDateTime ts = entry.timestamp.isValid() ? entry.timestamp : QDateTime::currentDateTime();
        if (!writePointEntry(query, entry, ts)) {
            setLastError(QStringLiteral("写入点位归档失败(point=%1): %2")
                             .arg(entry.pointId).arg(query.lastError().text()));
            qWarning() << "HistoryData::addPointRecords:" << m_lastError;
            if (ownTransaction)
                m_db.rollback();
            return false;
        }
    }
    if (ownTransaction && !m_db.commit()) {
        setLastError(QStringLiteral("提交点位归档事务失败: %1").arg(m_db.lastError().text()));
        return false;
    }

    setLastError(QString());
    maybeAutoCleanup();
    return true;
}

QVector<PointHistoryRecord> HistoryData::queryPoints(const QDateTime &from, const QDateTime &to,
                                                     int deviceId, int pointId,
                                                     int limit, int offset) const
{
    QString sql = "SELECT * FROM point_history WHERE timestamp >= ? AND timestamp <= ?";
    QVariantList binds;
    binds.append(from.toString(Qt::ISODateWithMs));
    binds.append(to.toString(Qt::ISODateWithMs));
    if (deviceId >= 0) {
        sql += " AND device_id = ?";
        binds.append(deviceId);
    }
    if (pointId >= 0) {
        sql += " AND point_id = ?";
        binds.append(pointId);
    }
    sql += " ORDER BY timestamp DESC";
    if (limit >= 0) {
        sql += " LIMIT ? OFFSET ?";
        binds.append(limit);
        binds.append(qMax(0, offset));
    }

    QSqlQuery q(m_db);
    q.prepare(sql);
    for (const QVariant &value : binds)
        q.addBindValue(value);
    if (!q.exec()) {
        setLastError(QStringLiteral("查询点位归档失败: %1").arg(q.lastError().text()));
        qWarning() << "HistoryData::queryPoints:" << m_lastError;
        return QVector<PointHistoryRecord>();
    }

    QVector<PointHistoryRecord> results;
    while (q.next()) {
        PointHistoryRecord rec;
        rec.id = q.value("id").toLongLong();
        rec.timestamp = QDateTime::fromString(q.value("timestamp").toString(), Qt::ISODateWithMs);
        rec.deviceId = q.value("device_id").toInt();
        rec.pointId = q.value("point_id").toInt();
        rec.pointName = q.value("point_name").toString();
        rec.serverAddress = q.value("server_address").toInt();
        rec.value = q.value("value").toDouble();
        rec.quality = q.value("quality").toInt();
        results.append(rec);
    }
    return results;
}

QVector<PointHistoryAggregate> HistoryData::aggregatePoints(const QDateTime &from, const QDateTime &to,
                                                            int deviceId, int pointId,
                                                            int bucketSeconds) const
{
    const int bucket = qMax(1, bucketSeconds);
    QString sql =
        "SELECT (CAST(strftime('%s', timestamp) AS INTEGER) / ?) * ? AS bucket, "
        "MIN(value), MAX(value), AVG(value), COUNT(*) "
        "FROM point_history WHERE timestamp >= ? AND timestamp <= ?";
    QVariantList binds;
    binds.append(bucket);
    binds.append(bucket);
    binds.append(from.toString(Qt::ISODateWithMs));
    binds.append(to.toString(Qt::ISODateWithMs));
    if (deviceId >= 0) {
        sql += " AND device_id = ?";
        binds.append(deviceId);
    }
    if (pointId >= 0) {
        sql += " AND point_id = ?";
        binds.append(pointId);
    }
    sql += " GROUP BY bucket ORDER BY bucket ASC";

    QSqlQuery q(m_db);
    q.prepare(sql);
    for (const QVariant &value : binds)
        q.addBindValue(value);
    if (!q.exec()) {
        setLastError(QStringLiteral("聚合点位归档失败: %1").arg(q.lastError().text()));
        qWarning() << "HistoryData::aggregatePoints:" << m_lastError;
        return QVector<PointHistoryAggregate>();
    }

    QVector<PointHistoryAggregate> results;
    while (q.next()) {
        PointHistoryAggregate agg;
        agg.bucketStart = QDateTime::fromSecsSinceEpoch(q.value(0).toLongLong());
        agg.minValue = q.value(1).toDouble();
        agg.maxValue = q.value(2).toDouble();
        agg.avgValue = q.value(3).toDouble();
        agg.count = q.value(4).toInt();
        results.append(agg);
    }
    return results;
}

void HistoryData::maybeAutoCleanup()
{
    if (m_retentionDays <= 0)
        return;   // 0 = 不自动清理
    if (++m_cleanupCounter < kCleanupInterval)
        return;
    m_cleanupCounter = 0;

    const QDateTime before = QDateTime::currentDateTime().addDays(-m_retentionDays);
    const int removed = purgeOlderThan(before);
    qInfo() << "HistoryData: 自动清理保留期" << m_retentionDays << "天，删除记录数:" << removed;
}

int HistoryData::purgeOlderThan(const QDateTime &before)
{
    if (!m_db.isOpen())
        return 0;

    const QString ts = before.toString(Qt::ISODateWithMs);
    const bool ownTransaction = m_db.transaction();
    int removed = 0;

    QSqlQuery q(m_db);
    q.prepare("DELETE FROM history WHERE timestamp < ?");
    q.addBindValue(ts);
    if (!q.exec()) {
        setLastError(QStringLiteral("清理历史记录失败: %1").arg(q.lastError().text()));
        qWarning() << "HistoryData::purgeOlderThan:" << m_lastError;
        if (ownTransaction)
            m_db.rollback();
        return 0;
    }
    removed += q.numRowsAffected();

    if (!q.prepare("DELETE FROM point_history WHERE timestamp < ?")) {
        setLastError(QStringLiteral("准备清理点位归档语句失败: %1").arg(q.lastError().text()));
        if (ownTransaction)
            m_db.rollback();
        return 0;
    }
    q.addBindValue(ts);
    if (!q.exec()) {
        setLastError(QStringLiteral("清理点位归档失败: %1").arg(q.lastError().text()));
        qWarning() << "HistoryData::purgeOlderThan:" << m_lastError;
        if (ownTransaction)
            m_db.rollback();
        return 0;
    }
    removed += q.numRowsAffected();

    if (ownTransaction && !m_db.commit()) {
        setLastError(QStringLiteral("提交清理事务失败: %1").arg(m_db.lastError().text()));
        return 0;
    }
    return removed;
}

QVector<HistoryRecord> HistoryData::query(const QDateTime &from, const QDateTime &to,
                                           int serverAddress, int registerType,
                                           int startAddress, int limit, int offset) const
{
    // P2-16b：整数条件一律用占位符绑定，杜绝将来改成字符串后的注入风险。
    // 绑定顺序必须与占位符顺序一致：timestamp 的两个占位符在最前。
    QString sql = "SELECT * FROM history WHERE timestamp >= ? AND timestamp <= ?";
    QVariantList binds;
    binds.append(from.toString(Qt::ISODateWithMs));
    binds.append(to.toString(Qt::ISODateWithMs));

    if (serverAddress >= 0) {
        sql += " AND server_address = ?";
        binds.append(serverAddress);
    }
    if (registerType >= 0) {
        sql += " AND register_type = ?";
        binds.append(registerType);
    }
    if (startAddress >= 0) {
        sql += " AND start_address = ?";
        binds.append(startAddress);
    }
    sql += " ORDER BY timestamp DESC";

    // P2-13：行数上限，避免一次把整个时间范围读进内存
    if (limit >= 0) {
        sql += " LIMIT ? OFFSET ?";
        binds.append(limit);
        binds.append(qMax(0, offset));
    }

    QSqlQuery q(m_db);
    q.prepare(sql);
    for (const QVariant &value : binds)
        q.addBindValue(value);
    if (!q.exec()) {
        setLastError(QStringLiteral("查询历史记录失败: %1").arg(q.lastError().text()));
        qWarning() << "HistoryData::query:" << m_lastError;
        return QVector<HistoryRecord>();
    }

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
    if (!q.exec()) {
        setLastError(QStringLiteral("查询最近记录失败: %1").arg(q.lastError().text()));
        return QVector<HistoryRecord>();
    }

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

qint64 HistoryData::totalRecords() const
{
    QSqlQuery q(m_db);
    q.exec("SELECT COUNT(*) FROM history");
    // P2-17：条数可能超过 2^31，toInt() 会溢出
    if (q.next())
        return q.value(0).toLongLong();
    return 0;
}

qint64 HistoryData::totalPointRecords() const
{
    QSqlQuery q(m_db);
    q.exec("SELECT COUNT(*) FROM point_history");
    if (q.next())
        return q.value(0).toLongLong();
    return 0;
}

void HistoryData::clearOlderThan(const QDateTime &before)
{
    purgeOlderThan(before);
}

void HistoryData::clearAll()
{
    if (!m_db.isOpen())
        return;
    QSqlQuery q(m_db);
    if (!q.exec("DELETE FROM history")) {
        setLastError(QStringLiteral("清空历史记录失败: %1").arg(q.lastError().text()));
        qWarning() << "HistoryData::clearAll:" << m_lastError;
        return;
    }
    if (!q.exec("DELETE FROM point_history")) {
        setLastError(QStringLiteral("清空点位归档失败: %1").arg(q.lastError().text()));
        qWarning() << "HistoryData::clearAll:" << m_lastError;
        return;
    }
    setLastError(QString());
}
