#ifndef DATAEXPORTER_H
#define DATAEXPORTER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QModbusDataUnit>

struct ExportRecord {
    QString timestamp;
    int serverAddress;
    QModbusDataUnit::RegisterType registerType;
    int startAddress;
    QVector<quint16> values;
};

class DataExporter : public QObject
{
    Q_OBJECT

public:
    explicit DataExporter(QObject *parent = nullptr);

    void addRecord(const ExportRecord &record);
    void setMaxRecords(int maxRecords);
    int maxRecords() const;
    bool exportToCsv(const QString &filePath) const;
    void clearRecords();
    int recordCount() const;
    QVector<ExportRecord> records() const;

private:
    QVector<ExportRecord> m_records;
    int m_maxRecords = 10000;
};

#endif // DATAEXPORTER_H
