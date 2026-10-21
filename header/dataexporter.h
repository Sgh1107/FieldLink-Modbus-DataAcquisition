#ifndef DATAEXPORTER_H
#define DATAEXPORTER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QQueue>
#include <QModbusDataUnit>

// 注意：所有字段必须带默认初值。
// 该结构体常以 `ExportRecord r;` 形式声明后逐字段赋值，若调用方漏设某个成员，
// 默认构造会留下未定值：switch(reg.registerType) 的跳转判定会读到垃圾值
// （Valgrind: Conditional jump depends on uninitialised value），
// QString::number(未定 int) 也会输出随机数字。默认值语义上取「最安全的只读表 + 从站1 + 地址0」。
struct ExportRecord {
    QString timestamp;
    int serverAddress = 1;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::HoldingRegisters;
    int startAddress = 0;
    QVector<quint16> values;
};

class DataExporter : public QObject
{
    Q_OBJECT

public:
    explicit DataExporter(QObject *parent = nullptr);

    void addRecord(const ExportRecord &record);
    // P1-/R-EXPORT-1：maxRecords <= 0 表示"使用默认上限"，不再表示"不限制"。
    // 超过硬上限时按硬上限截断，避免内存无界增长。
    void setMaxRecords(int maxRecords);
    int maxRecords() const;
    static int defaultMaxRecords() { return 10000; }
    static int hardMaxRecords() { return 1000000; }
    // errorMessage 可为 nullptr（保持既有调用点不变）；导出失败时填入错误描述
    bool exportToCsv(const QString &filePath, QString *errorMessage = nullptr) const;
    void clearRecords();
    int recordCount() const;
    QVector<ExportRecord> records() const;

private:
    void trimRecords();

    // QQueue：环形缓冲，出队 O(1)，避免 QVector::removeFirst() 的整体前移开销
    QQueue<ExportRecord> m_records;
    int m_maxRecords = 10000;
};

#endif // DATAEXPORTER_H
