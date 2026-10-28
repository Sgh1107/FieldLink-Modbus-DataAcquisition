#ifndef POINTMODEL_H
#define POINTMODEL_H

#include <QObject>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>
#include <QModbusDataUnit>

#include "dataparser.h"

enum class DataQuality {
    Good = 0,
    Bad,
    Timeout,
    OutOfRange,
    Unknown
};

struct PointDefinition {
    int id = 0;
    // 0 = 主连接（界面顶部的单连接）；>0 = DeviceManager 中的设备 id。
    // 点位/历史/报警必须带设备维度，否则两台同为从站 1 的设备会互相串数据。
    int deviceId = 0;
    QString name;
    int serverAddress = 1;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::HoldingRegisters;
    int address = 0;
    int count = 1;
    QString dataType = QStringLiteral("uint16");
    // 32 位及以上类型（uint32/int32/float32/uint64/int64/float64）的字节序。
    // 缺省为 ABCD（Modbus 标准大端分组）。缺失该字段会导致 DCBA/BADC/CDAB
    // 设备的 32 位数据被按 ABCD 解码 —— 数值错误但质量码仍为 Good（静默错误）。
    ByteOrder byteOrder = ByteOrder::BigEndian_ABCD;
    double scale = 1.0;
    double offset = 0.0;
    QString unit;
    double alarmLow = 0.0;
    double alarmHigh = 65535.0;
    bool archiveEnabled = true;
    int archiveIntervalSec = 5;
    // 归档死区（工程值）。0 = 每个归档周期都记录；>0 = 变化小于该值时不记录。
    double archiveDeadband = 0.0;
    // 采集周期。与归档周期解耦：此前点位轮询周期直接借用 archiveIntervalSec*1000，
    // 想提高归档频率就不得不提高采集频率（或反之）。
    int pollIntervalSec = 1;
};

struct PointValue {
    int pointId = 0;
    double value = 0.0;
    DataQuality quality = DataQuality::Unknown;
    QDateTime timestamp;
};

class PointModel : public QObject
{
    Q_OBJECT

public:
    explicit PointModel(QObject *parent = nullptr);

    int addPoint(const PointDefinition &point);
    void removePoint(int id);
    void updatePoint(const PointDefinition &point);
    QVector<PointDefinition> points() const;
    PointDefinition point(int id) const;

    PointValue convertRawValue(const PointDefinition &point, const QVector<quint16> &rawValues) const;
    // deviceId 放在末尾并带默认值：既有调用/测试保持兼容，默认仍为主连接。
    void updateFromRaw(int serverAddress, QModbusDataUnit::RegisterType type, int startAddress,
                       const QVector<quint16> &values, int deviceId = 0);
    QVector<PointValue> currentValues() const;
    PointValue value(int pointId) const;

    // ---------- 通信质量降级 ----------
    // 轮询任务失败：把该任务关联的点位标为 Bad/Timeout。
    void markPointsBad(const QVector<int> &pointIds, DataQuality quality, const QString &reason = QString());
    // 兼容没有 pointIds 的手工任务：按设备+从站+寄存器类型+地址区间匹配点位。
    void markRangeBad(int deviceId, int serverAddress, QModbusDataUnit::RegisterType type,
                      int startAddress, int count, DataQuality quality, const QString &reason = QString());
    // 设备断开：该设备下所有点位降级。
    void markDeviceBad(int deviceId, DataQuality quality, const QString &reason = QString());
    // 陈旧检测：超过 max(minTimeoutMs, 3×pollInterval) 未更新的点位置 Timeout。
    // 返回本次降级的点位数。
    int markStale(const QDateTime &now, int minTimeoutMs = 5000);

    bool saveToFile(const QString &filePath) const;
    bool loadFromFile(const QString &filePath);

    // ---------- 点表批量导入 / 导出 ----------
    // 说明：id 在导入时一律重新分配；append=true 追加到现有点位，false 覆盖全部。
    bool exportToJson(const QString &filePath, QString *error = nullptr) const;
    bool exportToCsv(const QString &filePath, QString *error = nullptr) const;
    bool importFromJson(const QString &filePath, bool append, int *imported = nullptr, QString *error = nullptr);
    bool importFromCsv(const QString &filePath, bool append, int *imported = nullptr, QString *error = nullptr);
    // 按扩展名自动选择 CSV / JSON
    bool importAuto(const QString &filePath, bool append, int *imported = nullptr, QString *error = nullptr);

    static QString registerTypeName(QModbusDataUnit::RegisterType type);

signals:
    void pointValueUpdated(const PointDefinition &point, const PointValue &value);
    void pointQualityChanged(int pointId, DataQuality quality);

private:
    QJsonObject pointToJson(const PointDefinition &point) const;
    PointDefinition pointFromJson(const QJsonObject &obj) const;

    QVector<PointDefinition> m_points;
    QVector<PointValue> m_values;
    int m_nextId;
};

#endif // POINTMODEL_H
