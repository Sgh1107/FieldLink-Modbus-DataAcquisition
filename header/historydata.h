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

struct HistoryRecord {
    qint64 id;
    QDateTime timestamp;
    int serverAddress;
    QModbusDataUnit::RegisterType registerType;
    int startAddress;
    int count;
    QVector<quint16> values;
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

    QVector<HistoryRecord> query(const QDateTime &from, const QDateTime &to,
                                  int serverAddress = -1,
                                  int registerType = -1,
                                  int startAddress = -1) const;

    QVector<HistoryRecord> lastRecords(int count = 100) const;
    int totalRecords() const;
    void clearOlderThan(const QDateTime &before);
    void clearAll();

private:
    void createTables();
    QSqlDatabase m_db;
    QString m_connectionName;
    int m_cleanupCounter = 0;   // H2：清理计数器改为成员变量（原函数级 static 多实例共享）
};

#endif // HISTORYDATA_H
