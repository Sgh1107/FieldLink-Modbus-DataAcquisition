#include "dataexporter.h"
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QLocale>
#include <QtGlobal>

namespace {

// U3：CSV 字段统一加引号并转义内部引号（双写），避免字段内容含逗号/引号破坏 CSV 结构。
// P2-19/P2-62：加引号本身不是 CSV 公式注入的防护手段——以 = + - @ 或 TAB/CR/LF
// 开头的单元格在 Excel 中会被当作公式执行，这里统一加 ' 前缀强制按文本处理。
QString csvField(const QString &raw)
{
    QString field = raw;
    if (!field.isEmpty()) {
        const QChar first = field.at(0);
        if (first == QLatin1Char('=') || first == QLatin1Char('+') || first == QLatin1Char('-')
            || first == QLatin1Char('@') || first == QLatin1Char('\t')
            || first == QLatin1Char('\r') || first == QLatin1Char('\n')) {
            field.prepend(QLatin1Char('\''));
        }
    }
    field.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + field + QLatin1Char('"');
}

} // namespace

DataExporter::DataExporter(QObject *parent) : QObject(parent) {}

void DataExporter::addRecord(const ExportRecord &record)
{
    m_records.enqueue(record);
    trimRecords();
}

void DataExporter::trimRecords()
{
    const int limit = qMax(1, m_maxRecords);
    // QQueue::dequeue() 为 O(1)：稳态下每插一条只搬移常数个元素
    while (m_records.size() > limit)
        m_records.dequeue();
}

void DataExporter::setMaxRecords(int maxRecords)
{
    // P1-/R-EXPORT-1：0 或负数曾经让裁剪条件短路（`m_maxRecords > 0`），
    // 结果一条都不裁，内存无界增长。现在明确语义：
    //   <= 0            -> 使用默认上限（10000）
    //   > 硬上限        -> 截断到硬上限（即使传 INT_MAX 也保留一道硬保护）
    if (maxRecords <= 0)
        m_maxRecords = defaultMaxRecords();
    else if (maxRecords > hardMaxRecords())
        m_maxRecords = hardMaxRecords();
    else
        m_maxRecords = maxRecords;

    trimRecords();
}

int DataExporter::maxRecords() const
{
    return m_maxRecords;
}

bool DataExporter::exportToCsv(const QString &filePath, QString *errorMessage) const
{
    if (errorMessage)
        errorMessage->clear();

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage)
            *errorMessage = file.errorString().isEmpty()
                ? QCoreApplication::translate("DataExporter", "Cannot open file: %1").arg(filePath)
                : file.errorString();
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    // P2-18：写入 UTF-8 BOM，Excel 打开中文不再乱码（与点表导出保持一致）
    out.setGenerateByteOrderMark(true);
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
            valStrs.append(QLocale::c().toString(v));

        out << csvField(rec.timestamp) << ","
            << csvField(QLocale::c().toString(rec.serverAddress)) << ","
            << csvField(typeStr) << ","
            << csvField(QLocale::c().toString(rec.startAddress)) << ","
            << csvField(valStrs.join(";")) << "\n";
    }

    // P2-18：检查写入结果，磁盘满/写入失败不能静默返回 true
    out.flush();
    const bool streamOk = (out.status() == QTextStream::Ok);
    const bool fileOk = (file.error() == QFile::NoError);
    if (!streamOk || !fileOk) {
        const QString reason = file.errorString().isEmpty()
            ? QCoreApplication::translate("DataExporter", "Failed to write CSV (stream error)")
            : file.errorString();
        if (errorMessage)
            *errorMessage = reason;
        file.close();
        return false;
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
    QVector<ExportRecord> result;
    result.reserve(m_records.size());
    for (const auto &rec : m_records)
        result.append(rec);
    return result;
}
