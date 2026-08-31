#include "dataexporter.h"
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QtGlobal>

DataExporter::DataExporter(QObject *parent) : QObject(parent) {}

void DataExporter::addRecord(const ExportRecord &record)
{
    m_records.append(record);
    while (m_maxRecords > 0 && m_records.size() > m_maxRecords)
        m_records.removeFirst();
}

void DataExporter::setMaxRecords(int maxRecords)
{
    m_maxRecords = qMax(0, maxRecords);
    while (m_maxRecords > 0 && m_records.size() > m_maxRecords)
        m_records.removeFirst();
}

int DataExporter::maxRecords() const
{
    return m_maxRecords;
}

bool DataExporter::exportToCsv(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out << "Timestamp,ServerAddress,RegisterType,StartAddress,Values\n";

    for (const auto &rec : m_records) {
        QString typeStr;
        switch (rec.registerType) {
        case QModbusDataUnit::Coils: typeStr = "Coils"; break;
        case QModbusDataUnit::DiscreteInputs: typeStr = "DiscreteInputs"; break;
        case QModbusDataUnit::InputRegisters: typeStr = "InputRegisters"; break;
        case QModbusDataUnit::HoldingRegisters: typeStr = "HoldingRegisters"; break;
        default: typeStr = "Unknown"; break;
        }

        QStringList valStrs;
        for (quint16 v : rec.values)
            valStrs.append(QString::number(v));

        out << rec.timestamp << ","
            << rec.serverAddress << ","
            << typeStr << ","
            << rec.startAddress << ","
            << "\"" << valStrs.join(";") << "\"\n";
    }

    file.close();
    return true;
}

void DataExporter::clearRecords()
{
    m_records.clear();
}

int DataExporter::recordCount() const
{
    return m_records.size();
}

QVector<ExportRecord> DataExporter::records() const
{
    return m_records;
}
