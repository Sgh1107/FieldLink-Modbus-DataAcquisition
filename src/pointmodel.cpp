#include "pointmodel.h"
#include "dataparser.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QTextStream>
#include <QHash>
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

// ==================== 点表批量导入 / 导出 ====================

namespace {

QString csvEscape(const QString &value)
{
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        QString s = value;
        s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + s + QLatin1Char('"');
    }
    return value;
}

// 支持双引号包裹与 "" 转义的 CSV 解析
QVector<QStringList> parseCsvText(const QString &text)
{
    QVector<QStringList> rows;
    QStringList row;
    QString cell;
    bool inQuotes = false;
    const int n = text.size();
    int i = 0;
    while (i < n) {
        const QChar ch = text.at(i);
        if (inQuotes) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < n && text.at(i + 1) == QLatin1Char('"')) {
                    cell.append(QLatin1Char('"'));
                    i += 2;
                    continue;
                }
                inQuotes = false;
                ++i;
                continue;
            }
            cell.append(ch);
            ++i;
            continue;
        }
        if (ch == QLatin1Char('"')) { inQuotes = true; ++i; continue; }
        if (ch == QLatin1Char(',')) { row << cell; cell.clear(); ++i; continue; }
        if (ch == QLatin1Char('\r')) { ++i; continue; }
        if (ch == QLatin1Char('\n')) { row << cell; cell.clear(); rows << row; row.clear(); ++i; continue; }
        cell.append(ch);
        ++i;
    }
    if (!cell.isEmpty() || !row.isEmpty()) {
        row << cell;
        rows << row;
    }
    return rows;
}

bool registerTypeFromToken(const QString &token, QModbusDataUnit::RegisterType *out)
{
    const QString t = token.trimmed();
    if (t.isEmpty())
        return false;
    bool ok = false;
    const int numeric = t.toInt(&ok);
    if (ok) {
        // 必须与 QModbusDataUnit::RegisterType 的真实枚举值严格一致：
        //   Invalid=0, DiscreteInputs=1, Coils=2, InputRegisters=3, HoldingRegisters=4
        // 导出路径写出的就是这些值（pointToJson / exportToCsv 均用 static_cast<int>），
        // 若这里按「表格习惯序号 0=线圈…3=保持寄存器」解析，导出的 Coils(2) 会被
        // 重新读成 InputRegisters —— 静默改变寄存器类型，属数据损坏。
        // 因此数字形式只接受 Qt 枚举值；Invalid 与未知值一律拒绝（回落默认保持寄存器）。
        switch (numeric) {
        case static_cast<int>(QModbusDataUnit::Coils):           *out = QModbusDataUnit::Coils; return true;
        case static_cast<int>(QModbusDataUnit::DiscreteInputs):   *out = QModbusDataUnit::DiscreteInputs; return true;
        case static_cast<int>(QModbusDataUnit::InputRegisters):  *out = QModbusDataUnit::InputRegisters; return true;
        case static_cast<int>(QModbusDataUnit::HoldingRegisters): *out = QModbusDataUnit::HoldingRegisters; return true;
        default: return false;   // 含 Invalid(0) 与越界值
        }
    }
    const QString low = t.toLower();
    if (low.contains(QStringLiteral("holding")) || low.contains(QStringLiteral("保持"))) { *out = QModbusDataUnit::HoldingRegisters; return true; }
    if (low.contains(QStringLiteral("input")) || low.contains(QStringLiteral("输入寄存器"))) { *out = QModbusDataUnit::InputRegisters; return true; }
    if (low.contains(QStringLiteral("discrete")) || low.contains(QStringLiteral("离散"))) { *out = QModbusDataUnit::DiscreteInputs; return true; }
    if (low.contains(QStringLiteral("coil")) || low.contains(QStringLiteral("线圈"))) { *out = QModbusDataUnit::Coils; return true; }
    return false;
}

bool boolFromToken(const QString &token, bool defaultValue)
{
    const QString t = token.trimmed().toLower();
    if (t.isEmpty())
        return defaultValue;
    if (t == QStringLiteral("1") || t == QStringLiteral("true") || t == QStringLiteral("yes")
        || t == QStringLiteral("y") || t == QStringLiteral("是") || t == QStringLiteral("on"))
        return true;
    if (t == QStringLiteral("0") || t == QStringLiteral("false") || t == QStringLiteral("no")
        || t == QStringLiteral("n") || t == QStringLiteral("否") || t == QStringLiteral("off"))
        return false;
    return defaultValue;
}

} // namespace

QString PointModel::registerTypeName(QModbusDataUnit::RegisterType type)
{
    switch (type) {
    case QModbusDataUnit::Coils: return QStringLiteral("Coils");
    case QModbusDataUnit::DiscreteInputs: return QStringLiteral("DiscreteInputs");
    case QModbusDataUnit::InputRegisters: return QStringLiteral("InputRegisters");
    case QModbusDataUnit::HoldingRegisters: return QStringLiteral("HoldingRegisters");
    default: return QStringLiteral("HoldingRegisters");
    }
}

bool PointModel::exportToJson(const QString &filePath, QString *error) const
{
    QJsonArray arr;
    for (const auto &p : m_points)
        arr.append(pointToJson(p));
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

bool PointModel::exportToCsv(const QString &filePath, QString *error) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true);
    out << "id,name,serverAddress,registerType,address,count,dataType,scale,offset,unit,alarmLow,alarmHigh,archiveEnabled,archiveIntervalSec\n";
    for (const auto &p : m_points) {
        QStringList cells;
        cells << QString::number(p.id)
              << csvEscape(p.name)
              << QString::number(p.serverAddress)
              << QString::number(static_cast<int>(p.registerType))
              << QString::number(p.address)
              << QString::number(p.count)
              << csvEscape(p.dataType)
              << QString::number(p.scale)
              << QString::number(p.offset)
              << csvEscape(p.unit)
              << QString::number(p.alarmLow)
              << QString::number(p.alarmHigh)
              << QString(p.archiveEnabled ? QLatin1String("1") : QLatin1String("0"))
              << QString::number(p.archiveIntervalSec);
        out << cells.join(QLatin1Char(',')) << '\n';
    }
    return true;
}

bool PointModel::importFromJson(const QString &filePath, bool append, int *imported, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取文件：%1").arg(file.errorString());
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error) *error = QStringLiteral("JSON 解析失败：%1").arg(parseError.errorString());
        return false;
    }
    // 原子性：先解析到临时列表，确认有内容后再改动现有数据（与 CSV 导入保持一致）
    QVector<PointDefinition> parsed;
    for (const auto &v : doc.array())
        parsed.append(pointFromJson(v.toObject()));

    if (!append) {
        m_points.clear();
        m_values.clear();
    }
    for (const PointDefinition &p : parsed)
        addPoint(p);
    if (imported) *imported = parsed.size();
    return true;
}

bool PointModel::importFromCsv(const QString &filePath, bool append, int *imported, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法读取文件：%1").arg(file.errorString());
        return false;
    }
    QTextStream in(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    in.setCodec("UTF-8");
#endif
    QString text = in.readAll();
    if (!text.isEmpty() && text.at(0) == QChar(0xFEFF))
        text.remove(0, 1);

    const QVector<QStringList> rawRows = parseCsvText(text);
    QVector<QStringList> rows;
    for (const QStringList &r : rawRows) {
        bool hasContent = false;
        for (const QString &c : r) {
            if (!c.trimmed().isEmpty()) { hasContent = true; break; }
        }
        if (hasContent)
            rows << r;
    }
    if (rows.isEmpty()) {
        // 空文件 / 只有空白：视为「无内容可导入」，不算失败（区别于「有内容但全部无效」）
        if (imported) *imported = 0;
        return true;
    }

    // 表头识别（含 name / serverAddress / registerType / dataType 任一即视为表头）
    QHash<QString, int> col;
    bool hasHeader = false;
    const QStringList headerRow = rows.first();
    for (int i = 0; i < headerRow.size(); ++i) {
        const QString key = headerRow.at(i).trimmed().toLower();
        if (key.isEmpty())
            continue;
        if (key == QLatin1String("name") || key == QLatin1String("serveraddress")
            || key == QLatin1String("registertype") || key == QLatin1String("datatype")) {
            hasHeader = true;
        }
        col.insert(key, i);
    }

    // 无表头时按导出顺序回退定位
    const QStringList defaultOrder = { "id", "name", "serveraddress", "registertype", "address",
                                       "count", "datatype", "scale", "offset", "unit",
                                       "alarmlow", "alarmhigh", "archiveenabled", "archiveintervalsec" };
    for (int i = 0; i < defaultOrder.size(); ++i) {
        if (!col.contains(defaultOrder.at(i)))
            col.insert(defaultOrder.at(i), i);
    }

    auto cellOf = [&col](const QStringList &row, const QString &key) -> QString {
        const int index = col.value(key, -1);
        if (index < 0 || index >= row.size())
            return QString();
        return row.at(index).trimmed();
    };

    // 原子性：先解析到临时列表，确认有有效点位后再改动现有数据。
    // 否则「覆盖模式导入一个全是无效行的 CSV」会先把用户现有点位清空再报错，造成数据丢失。
    QVector<PointDefinition> parsed;
    int skipped = 0;
    const int startRow = hasHeader ? 1 : 0;
    for (int r = startRow; r < rows.size(); ++r) {
        const QStringList &row = rows.at(r);
        PointDefinition p;
        p.name = cellOf(row, QStringLiteral("name"));
        const QString addrText = cellOf(row, QStringLiteral("address"));
        if (p.name.isEmpty() && addrText.isEmpty()) {
            ++skipped;
            continue;
        }
        p.serverAddress = cellOf(row, QStringLiteral("serveraddress")).toInt();
        if (p.serverAddress <= 0)
            p.serverAddress = 1;
        QModbusDataUnit::RegisterType type = QModbusDataUnit::HoldingRegisters;
        if (registerTypeFromToken(cellOf(row, QStringLiteral("registertype")), &type))
            p.registerType = type;
        p.address = addrText.toInt();
        p.count = qMax(1, cellOf(row, QStringLiteral("count")).toInt());
        const QString dt = cellOf(row, QStringLiteral("datatype"));
        p.dataType = dt.isEmpty() ? QStringLiteral("uint16") : dt;
        const QString scaleText = cellOf(row, QStringLiteral("scale"));
        p.scale = scaleText.isEmpty() ? 1.0 : scaleText.toDouble();
        const QString offsetText = cellOf(row, QStringLiteral("offset"));
        p.offset = offsetText.isEmpty() ? 0.0 : offsetText.toDouble();
        p.unit = cellOf(row, QStringLiteral("unit"));
        p.alarmLow = cellOf(row, QStringLiteral("alarmlow")).toDouble();
        const QString highText = cellOf(row, QStringLiteral("alarmhigh"));
        p.alarmHigh = highText.isEmpty() ? 65535.0 : highText.toDouble();
        if (p.alarmHigh <= p.alarmLow)
            p.alarmHigh = 65535.0;
        p.archiveEnabled = boolFromToken(cellOf(row, QStringLiteral("archiveenabled")), true);
        const int archiveInterval = cellOf(row, QStringLiteral("archiveintervalsec")).toInt();
        p.archiveIntervalSec = qMax(1, archiveInterval > 0 ? archiveInterval : 5);
        if (p.name.isEmpty())
            p.name = QStringLiteral("点位%1").arg(p.address);
        parsed.append(p);
    }

    if (parsed.isEmpty()) {
        // 一条有效点位都没有 → 视为导入失败，且不触碰现有点位
        if (imported) *imported = 0;
        if (error) *error = QStringLiteral("未解析到有效点位（共 %1 行被跳过），请检查表头或列顺序。").arg(skipped);
        return false;
    }

    if (!append) {
        m_points.clear();
        m_values.clear();
    }
    for (const PointDefinition &p : parsed)
        addPoint(p);

    if (imported) *imported = parsed.size();
    return true;
}

bool PointModel::importAuto(const QString &filePath, bool append, int *imported, QString *error)
{
    if (filePath.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
        return importFromCsv(filePath, append, imported, error);
    if (filePath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        return importFromJson(filePath, append, imported, error);
    // 未知扩展名：先按 JSON 试，失败再按 CSV 试。
    // 两次尝试都必须走「非覆盖」的安全路径：若调用方要求覆盖，而最终失败，
    // 现有点位必须完好 —— 否则「探测格式」本身就可能清空用户数据。
    if (append) {
        // 追加模式：直接依次尝试，失败不涉及覆盖，天然安全
        QString firstError;
        if (importFromJson(filePath, true, imported, &firstError))
            return true;
        return importFromCsv(filePath, true, imported, error);
    }

    // 覆盖模式：先探测，再落地
    PointModel probe;
    int probeCount = 0;
    QString jsonError;
    if (probe.importFromJson(filePath, true, &probeCount, &jsonError)) {
        const bool ok = importFromJson(filePath, false, imported, error);
        return ok;
    }
    QString csvError;
    if (probe.importFromCsv(filePath, true, &probeCount, &csvError)) {
        const bool ok = importFromCsv(filePath, false, imported, error);
        return ok;
    }
    // 两种格式都解析失败：给出合并后的错误信息，且不改动现有点位
    if (error)
        *error = QStringLiteral("无法识别文件格式（既非 JSON 也非 CSV）。JSON 解析：%1；CSV 解析：%2")
                     .arg(jsonError, csvError);
    if (imported) *imported = 0;
    return false;
}
