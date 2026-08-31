#ifndef POINTMODEL_H
#define POINTMODEL_H

#include <QObject>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>
#include <QModbusDataUnit>

enum class DataQuality {
    Good = 0,
    Bad,
    Timeout,
    OutOfRange,
    Unknown
};

struct PointDefinition {
    int id = 0;
    QString name;
    int serverAddress = 1;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::HoldingRegisters;
    int address = 0;
    int count = 1;
    QString dataType = QStringLiteral("uint16");
    double scale = 1.0;
    double offset = 0.0;
    QString unit;
    double alarmLow = 0.0;
    double alarmHigh = 65535.0;
    bool archiveEnabled = true;
    int archiveIntervalSec = 5;
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
    void updateFromRaw(int serverAddress, QModbusDataUnit::RegisterType type, int startAddress, const QVector<quint16> &values);
    QVector<PointValue> currentValues() const;

    bool saveToFile(const QString &filePath) const;
    bool loadFromFile(const QString &filePath);

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
