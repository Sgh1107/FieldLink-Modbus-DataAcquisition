#include "pointmodel.h"
#include "dataparser.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QtGlobal>

PointModel::PointModel(QObject *parent)
    : QObject(parent)
    , m_nextId(1)
{
}

int PointModel::addPoint(const PointDefinition &point)
{
    PointDefinition p = point;
    p.id = m_nextId++;
    m_points.append(p);
    PointValue v;
    v.pointId = p.id;
    v.quality = DataQuality::Unknown;
    m_values.append(v);
    return p.id;
}

void PointModel::removePoint(int id)
{
    for (int i = 0; i < m_points.size(); ++i) {
        if (m_points[i].id == id) {
            m_points.removeAt(i);
            break;
        }
    }
    for (int i = 0; i < m_values.size(); ++i) {
        if (m_values[i].pointId == id) {
            m_values.removeAt(i);
            break;
        }
    }
}

void PointModel::updatePoint(const PointDefinition &point)
{
    for (auto &p : m_points) {
        if (p.id == point.id) {
            p = point;
            return;
        }
    }
}

QVector<PointDefinition> PointModel::points() const { return m_points; }

PointDefinition PointModel::point(int id) const
{
    for (const auto &p : m_points) {
        if (p.id == id)
            return p;
    }
    return PointDefinition();
}

PointValue PointModel::convertRawValue(const PointDefinition &point, const QVector<quint16> &rawValues) const
{
    PointValue result;
    result.pointId = point.id;
    result.timestamp = QDateTime::currentDateTime();
    result.quality = DataQuality::Good;

    if (rawValues.isEmpty()) {
        result.quality = DataQuality::Bad;
        return result;
    }

    double raw = rawValues.first();
    if (point.dataType == "int16") {
        raw = static_cast<qint16>(rawValues.first());
    } else if ((point.dataType == "uint32" || point.dataType == "int32" || point.dataType == "float32") && rawValues.size() >= 2) {
        if (point.dataType == "float32")
            raw = DataParser::toFloat32(rawValues[0], rawValues[1]);
        else if (point.dataType == "int32")
            raw = DataParser::toInt32(rawValues[0], rawValues[1]);
        else
            raw = DataParser::toUInt32(rawValues[0], rawValues[1]);
    }

    result.value = raw * point.scale + point.offset;
    if (result.value < point.alarmLow || result.value > point.alarmHigh)
        result.quality = DataQuality::OutOfRange;
    return result;
}

void PointModel::updateFromRaw(int serverAddress, QModbusDataUnit::RegisterType type, int startAddress, const QVector<quint16> &values)
{
    for (const auto &point : m_points) {
        if (point.serverAddress != serverAddress || point.registerType != type)
            continue;
        if (point.address < startAddress || point.address + point.count > startAddress + values.size())
            continue;

        const int offset = point.address - startAddress;
        QVector<quint16> slice;
        for (int i = 0; i < point.count && offset + i < values.size(); ++i)
            slice.append(values[offset + i]);

        PointValue value = convertRawValue(point, slice);
        for (auto &v : m_values) {
            if (v.pointId == point.id) {
                if (v.quality != value.quality)
                    emit pointQualityChanged(point.id, value.quality);
                v = value;
                break;
            }
        }
        emit pointValueUpdated(point, value);
    }
}

QVector<PointValue> PointModel::currentValues() const { return m_values; }

QJsonObject PointModel::pointToJson(const PointDefinition &point) const
{
    QJsonObject obj;
    obj["name"] = point.name;
    obj["serverAddress"] = point.serverAddress;
    obj["registerType"] = static_cast<int>(point.registerType);
    obj["address"] = point.address;
    obj["count"] = point.count;
    obj["dataType"] = point.dataType;
    obj["scale"] = point.scale;
    obj["offset"] = point.offset;
    obj["unit"] = point.unit;
    obj["alarmLow"] = point.alarmLow;
    obj["alarmHigh"] = point.alarmHigh;
    obj["archiveEnabled"] = point.archiveEnabled;
    obj["archiveIntervalSec"] = point.archiveIntervalSec;
    return obj;
}

PointDefinition PointModel::pointFromJson(const QJsonObject &obj) const
{
    PointDefinition point;
    point.name = obj["name"].toString();
    point.serverAddress = obj["serverAddress"].toInt(1);
    point.registerType = static_cast<QModbusDataUnit::RegisterType>(obj["registerType"].toInt(QModbusDataUnit::HoldingRegisters));
    point.address = obj["address"].toInt(0);
    point.count = obj["count"].toInt(1);
    point.dataType = obj["dataType"].toString("uint16");
    point.scale = obj["scale"].toDouble(1.0);
    point.offset = obj["offset"].toDouble(0.0);
    point.unit = obj["unit"].toString();
    point.alarmLow = obj["alarmLow"].toDouble(0.0);
    point.alarmHigh = obj["alarmHigh"].toDouble(65535.0);
    point.archiveEnabled = obj["archiveEnabled"].toBool(true);
    point.archiveIntervalSec = obj["archiveIntervalSec"].toInt(5);
    return point;
}

bool PointModel::saveToFile(const QString &filePath) const
{
    QJsonArray arr;
    for (const auto &p : m_points)
        arr.append(pointToJson(p));
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

bool PointModel::loadFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return false;
    m_points.clear();
    m_values.clear();
    for (const auto &v : doc.array())
        addPoint(pointFromJson(v.toObject()));
    return true;
}
