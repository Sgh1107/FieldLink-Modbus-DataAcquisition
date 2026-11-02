#include "writeguard.h"

#include <QCoreApplication>

int WriteGuard::registerSpan(const PointDefinition &point)
{
    int span = qMax(1, point.count);
    const QString type = point.dataType.trimmed().toLower();
    int needed = 1;
    if (type == QLatin1String("uint32") || type == QLatin1String("int32")
        || type == QLatin1String("float32")) {
        needed = 2;
    } else if (type == QLatin1String("uint64") || type == QLatin1String("int64")
               || type == QLatin1String("float64")) {
        needed = 4;
    }
    return qMax(span, needed);
}

bool WriteGuard::overlaps(const PointDefinition &point, int deviceId, int serverAddress,
                          QModbusDataUnit::RegisterType type, int startAddress, int count)
{
    if (point.deviceId != deviceId || point.serverAddress != serverAddress
        || point.registerType != type) {
        return false;
    }
    if (count <= 0)
        return false;
    const int pointStart = point.address;
    const int pointEnd = point.address + registerSpan(point) - 1;
    const int writeEnd = startAddress + count - 1;
    return pointStart <= writeEnd && startAddress <= pointEnd;
}

QString WriteGuard::qualityName(DataQuality quality)
{
    switch (quality) {
    case DataQuality::Good:
        return QCoreApplication::translate("WriteGuard", "Good");
    case DataQuality::Bad:
        return QCoreApplication::translate("WriteGuard", "Bad");
    case DataQuality::Timeout:
        return QCoreApplication::translate("WriteGuard", "Timeout");
    case DataQuality::OutOfRange:
        return QCoreApplication::translate("WriteGuard", "OutOfRange");
    case DataQuality::Unknown:
    default:
        return QCoreApplication::translate("WriteGuard", "Unknown");
    }
}

WriteGuardResult WriteGuard::check(const QVector<PointDefinition> &points,
                                   const QVector<PointValue> &values,
                                   int deviceId, int serverAddress,
                                   QModbusDataUnit::RegisterType type,
                                   int startAddress, int count)
{
    WriteGuardResult result;

    auto findValue = [&values](int pointId) -> const PointValue * {
        for (const PointValue &v : values) {
            if (v.pointId == pointId)
                return &v;
        }
        return nullptr;
    };
    auto findPoint = [&points](int pointId) -> const PointDefinition * {
        for (const PointDefinition &p : points) {
            if (p.id == pointId)
                return &p;
        }
        return nullptr;
    };

    for (const PointDefinition &point : points) {
        if (!overlaps(point, deviceId, serverAddress, type, startAddress, count))
            continue;
        result.pointIds.append(point.id);

        // 1) 写保护优先于联锁：被保护的点位无论如何都不允许写。
        if (point.writeProtected) {
            result.allowed = false;
            result.blockedByProtection = true;
            result.reason = QCoreApplication::translate(
                                "WriteGuard",
                                "Point '%1' is write-protected; write rejected.")
                                .arg(point.name);
            return result;
        }

        // 2) 联锁：要求关联点位当前工程值处于允许区间。
        if (point.interlockEnabled) {
            const PointDefinition *linked = findPoint(point.interlockPointId);
            if (!linked) {
                result.allowed = false;
                result.blockedByInterlock = true;
                result.reason = QCoreApplication::translate(
                                    "WriteGuard",
                                    "Interlock point #%1 for '%2' no longer exists; write rejected.")
                                    .arg(point.interlockPointId)
                                    .arg(point.name);
                return result;
            }

            const PointValue *linkedValue = findValue(linked->id);
            if (!linkedValue || !linkedValue->timestamp.isValid()) {
                result.allowed = false;
                result.blockedByInterlock = true;
                result.reason = QCoreApplication::translate(
                                    "WriteGuard",
                                    "Interlock point '%1' has no valid reading; write rejected.")
                                    .arg(linked->name);
                return result;
            }
            if (linkedValue->quality != DataQuality::Good) {
                result.allowed = false;
                result.blockedByInterlock = true;
                result.reason = QCoreApplication::translate(
                                    "WriteGuard",
                                    "Interlock point '%1' quality is %2; write rejected.")
                                    .arg(linked->name)
                                    .arg(qualityName(linkedValue->quality));
                return result;
            }
            if (linkedValue->value < point.interlockMin
                || linkedValue->value > point.interlockMax) {
                result.allowed = false;
                result.blockedByInterlock = true;
                result.reason = QCoreApplication::translate(
                                    "WriteGuard",
                                    "Interlock requires '%1' in [%2, %3], current value is %4; write rejected.")
                                    .arg(linked->name)
                                    .arg(point.interlockMin, 0, 'g', 10)
                                    .arg(point.interlockMax, 0, 'g', 10)
                                    .arg(linkedValue->value, 0, 'g', 10);
                return result;
            }
        }
    }

    return result;
}
