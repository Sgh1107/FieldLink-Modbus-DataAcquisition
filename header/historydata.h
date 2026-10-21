#ifndef HISTORYDATA_H
#define HISTORYDATA_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QDateTime>
#include <QVariant>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QModbusDataUnit>

// 标量成员带默认值：记录会经 QVector 批量存储与筛选，未初始化成员会流入 SQL 绑定
struct HistoryRecord {
    qint64 id = 0;
    QDateTime timestamp;
    int serverAddress = 1;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::HoldingRegisters;
    int startAddress = 0;
    int count = 0;
    QVector<quint16> values;
};

// 批量写入的单条记录。timestamp 无效时使用当前时间。
struct HistoryBatchEntry {
    int serverAddress = 1;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::HoldingRegisters;
    int startAddress = 0;
    QVector<quint16> values;
    QDateTime timestamp;
};

class HistoryData : public QObject
{
    Q_OBJECT

public:
    explicit HistoryData(QObject *parent = nullptr);
    ~HistoryData();

    bool openDatabase(const QString &dbPath);
    void closeDatabase();
    bool isOpen() const;

    void addRecord(int serverAddress, QModbusDataUnit::RegisterType regType,
                   int startAddress, const QVector<quint16> &values);

    // P2-11：批量写入，用一个事务提交，避免逐条 autocommit 造成的写放大
    bool addRecords(const QVector<HistoryBatchEntry> &records);

    // limit < 0 表示不限制；offset 为跳过的行数
    QVector<HistoryRecord> query(const QDateTime &from, const QDateTime &to,
                                  int serverAddress = -1,
                                  int registerType = -1,
                                  int startAddress = -1,
                                  int limit = -1,
                                  int offset = 0) const;

    QVector<HistoryRecord> lastRecords(int count = 100) const;
    // 使用 qint64：条数超过 2^31 时 toInt() 会溢出
    qint64 totalRecords() const;
    void clearOlderThan(const QDateTime &before);
    void clearAll();

    // P2-12：最后一次失败的错误描述（打开失败/写入失败/提交失败）
    QString lastError() const;

    // P2-14：自动清理保留期（天）。0 = 不自动清理；负值同样视为关闭。
    int retentionDays() const;
    void setRetentionDays(int days);
    static int defaultRetentionDays() { return 30; }

private:
    void createTables();
    void applyPragmas();
    bool writeEntry(QSqlQuery &query, const HistoryBatchEntry &entry, const QDateTime &timestamp);
    void maybeAutoCleanup();
    int purgeOlderThan(const QDateTime &before);
    void setLastError(const QString &message) const;

    QSqlDatabase m_db;
    QString m_connectionName;
    int m_cleanupCounter = 0;   // H2：清理计数器改为成员变量（原函数级 static 多实例共享）
    int m_retentionDays = 30;   // P2-14：保留期可配置
    mutable QString m_lastError; // P2-12：最近一次错误描述
};

#endif // HISTORYDATA_H
