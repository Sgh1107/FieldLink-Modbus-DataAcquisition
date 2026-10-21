#include "pointmodel.h"
#include "dataparser.h"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QTextStream>
#include <QHash>
#include <QtGlobal>
#include <QDebug>

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

// 数据类型所需的寄存器数量。
// 返回 -1 表示"长度由 count 决定"（ascii 等字符串类型）。
static int requiredRegisterCount(const QString &dataType)
{
    if (dataType == "uint16" || dataType == "int16")
        return 1;
    if (dataType == "uint32" || dataType == "int32" || dataType == "float32")
        return 2;
    if (dataType == "uint64" || dataType == "int64" || dataType == "float64")
        return 4;
    return -1;   // ascii / 未知：由 count 决定
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

    const QString &dt = point.dataType;
    const int need = requiredRegisterCount(dt);

    // 寄存器数量不足以解码 → 质量码置 Bad（而不是静默降级成 16 位无符号值）。
    // 之前的行为是显示一个"看起来正常"的整数且质量码仍为 Good，现场无法察觉。
    if (need > 0 && rawValues.size() < need) {
        result.quality = DataQuality::Bad;
        qWarning("PointModel: point '%s' (id=%d) type=%s needs %d register(s) but only %lld available",
                 point.name.toUtf8().constData(), point.id, dt.toUtf8().constData(),
                 need, static_cast<long long>(rawValues.size()));
        return result;
    }

    double raw = rawValues.first();
    if (dt == "int16") {
        raw = static_cast<qint16>(rawValues.first());
    } else if (dt == "ascii") {
        raw = DataParser::toByteString(rawValues).toDouble();
    } else if (dt == "float32") {
        raw = DataParser::toFloat32(rawValues[0], rawValues[1], point.byteOrder);
    } else if (dt == "int32") {
        raw = DataParser::toInt32(rawValues[0], rawValues[1], point.byteOrder);
    } else if (dt == "uint32") {
        raw = DataParser::toUInt32(rawValues[0], rawValues[1], point.byteOrder);
    } else if (dt == "float64" && rawValues.size() >= 4) {
        raw = DataParser::toFloat64(rawValues[0], rawValues[1], rawValues[2], rawValues[3], point.byteOrder);
    } else if (dt == "int64" && rawValues.size() >= 4) {
        raw = static_cast<qint64>(DataParser::toUInt64(rawValues[0], rawValues[1], rawValues[2], rawValues[3], point.byteOrder));
    } else if (dt == "uint64" && rawValues.size() >= 4) {
        raw = static_cast<double>(DataParser::toUInt64(rawValues[0], rawValues[1], rawValues[2], rawValues[3], point.byteOrder));
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

        const int offset = point.address - startAddress;
        // 点位起点不在本次响应范围内 → 完全不涉及，跳过（正常情况）。
        if (offset < 0 || offset >= values.size())
            continue;

        // 点位所需寄存器超出本次响应范围（跨段/跨边界）：
        // 之前直接 continue，导致该点永远不更新、无 Bad 码、无日志 —— 界面保留旧值
        // 且看起来"正常"。现在明确置 Bad 并告警，让问题可见。
        if (point.address + point.count > startAddress + values.size()) {
            const int available = values.size() - offset;
            PointValue bad;
            bad.pointId = point.id;
            bad.timestamp = QDateTime::currentDateTime();
            bad.quality = DataQuality::Bad;
            for (auto &v : m_values) {
                if (v.pointId == point.id) {
                    if (v.quality != bad.quality)
                        emit pointQualityChanged(point.id, bad.quality);
                    v = bad;
                    break;
                }
            }
            qWarning("PointModel: point '%s' (id=%d, addr=%d, count=%d) crosses response boundary "
                     "(available %d register(s)) - marked Bad",
                     qPrintable(point.name), point.id, point.address, point.count, available);
            continue;
        }

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
    obj["byteOrder"] = DataParser::byteOrderToToken(point.byteOrder);
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
    {
        bool ok = false;
        const ByteOrder bo = DataParser::byteOrderFromToken(obj["byteOrder"].toString(), &ok);
        point.byteOrder = bo;   // 缺失或无法识别时回退 ABCD（Modbus 标准）
    }
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
    // QSaveFile：原子替换。直接用 QFile 会在 open(WriteOnly) 时立即截断原文件，
    // 写到一半崩溃/断电就留下半截 JSON → 下次加载失败 → 配置全丢且无法恢复。
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return file.commit();
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
    QString s = value;
    // CSV 公式注入防护：以 = + - @ 或 TAB/CR/LF 开头的单元格，Excel 打开时
    // 会当作公式求值（=cmd|'/c calc'!A1 可执行命令）。加 ' 前缀强制为文本。
    // 注意：给单元格加双引号**不是**防护手段 —— Excel 对引号内的公式照样求值。
    if (!s.isEmpty()) {
        const QChar c = s.at(0);
        if (c == QLatin1Char('=') || c == QLatin1Char('+') || c == QLatin1Char('-')
            || c == QLatin1Char('@') || c == QLatin1Char('\t') || c == QLatin1Char('\r')
            || c == QLatin1Char('\n')) {
            s.prepend(QLatin1Char('\''));
        }
    }
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"'))
        || s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'))) {
        s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + s + QLatin1Char('"');
    }
    return s;
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
        //   Invalid=0, Coils=1, DiscreteInputs=2, InputRegisters=3, HoldingRegisters=4
        // 导出路径写出的就是这些值（pointToJson / exportToCsv 均用 static_cast<int>），
        // 若这里按「表格习惯序号 0=线圈…3=保持寄存器」解析，导出的 Coils(1) 会被
        // 重新读成其它类型 —— 静默改变寄存器类型，属数据损坏。
        // 因此数字形式只接受 Qt 枚举值；Invalid 与未知值一律拒绝（回落默认保持寄存器）。
        switch (numeric) {
        case static_cast<int>(QModbusDataUnit::Coils):           *out = QModbusDataUnit::Coils; return true;
        case static_cast<int>(QModbusDataUnit::DiscreteInputs):   *out = QModbusDataUnit::DiscreteInputs; return true;
        case static_cast<int>(QModbusDataUnit::InputRegisters):  *out = QModbusDataUnit::InputRegisters; return true;
        case static_cast<int>(QModbusDataUnit::HoldingRegisters): *out = QModbusDataUnit::HoldingRegisters; return true;
        default: return false;   // 含 Invalid(0) 与越界值
        }
    }
    // 文本形式：必须精确匹配，不能用 contains() 模糊匹配。
    // 反例："discrete inputs" 里含有 "input"，模糊匹配会先命中 InputRegisters 分支，
    // 把离散输入的点位解析成输入寄存器 —— 数据完全错位且不报错。
    // 精确匹配下裸词 "input"/"discrete"/"coil" 各自唯一，语义明确，可以安全支持。
    const QString low = t.toLower();
    static const struct { const char *key; QModbusDataUnit::RegisterType type; } kTextMap[] = {
        { "coils",           QModbusDataUnit::Coils },
        { "coil",            QModbusDataUnit::Coils },
        { "discreteinputs",  QModbusDataUnit::DiscreteInputs },
        { "discrete inputs", QModbusDataUnit::DiscreteInputs },
        { "discrete",        QModbusDataUnit::DiscreteInputs },
        { "inputregisters",  QModbusDataUnit::InputRegisters },
        { "input registers", QModbusDataUnit::InputRegisters },
        { "input",           QModbusDataUnit::InputRegisters },
        { "holdingregisters",QModbusDataUnit::HoldingRegisters },
        { "holding registers",QModbusDataUnit::HoldingRegisters },
        { "holding",         QModbusDataUnit::HoldingRegisters },
        { "线圈",             QModbusDataUnit::Coils },
        { "离散输入",         QModbusDataUnit::DiscreteInputs },
        { "输入寄存器",       QModbusDataUnit::InputRegisters },
        { "保持寄存器",       QModbusDataUnit::HoldingRegisters },
    };
    for (const auto &e : kTextMap) {
        // 必须用 QString::fromUtf8：中文别名在源文件里是 UTF-8 字节，
        // 用 QLatin1String 比较会按 Latin-1 逐字节解释，永远匹配不上。
        if (low == QString::fromUtf8(e.key)) {
            *out = e.type;
            return true;
        }
    }
    return false;
}

// 点表列名的规范名 → CSV 表头别名。
// 本项目 UI 全中文，用户自制中文表头 CSV 是极可能场景；
// 只认英文表头会把中文表头当成数据行，生成一批垃圾点位。
struct ColumnAlias { const char *canonical; const char *alias; };

const ColumnAlias kColumnAliases[] = {
    { "name",               "name" },
    { "serveraddress",      "serveraddress" },
    { "registertype",       "registertype" },
    { "address",            "address" },
    { "count",              "count" },
    { "datatype",           "datatype" },
    { "byteorder",          "byteorder" },
    { "scale",              "scale" },
    { "offset",             "offset" },
    { "unit",               "unit" },
    { "alarmlow",           "alarmlow" },
    { "alarmhigh",          "alarmhigh" },
    { "archiveenabled",     "archiveenabled" },
    { "archiveintervalsec", "archiveintervalsec" },
    // 中文别名
    { "name",               "名称" },
    { "serveraddress",      "从站地址" },
    { "serveraddress",      "从站" },
    { "registertype",       "寄存器类型" },
    { "address",            "地址" },
    { "address",            "寄存器地址" },
    { "count",              "数量" },
    { "count",              "寄存器数量" },
    { "datatype",           "数据类型" },
    { "byteorder",          "字节序" },
    { "scale",              "系数" },
    { "scale",              "比例" },
    { "offset",             "偏移" },
    { "unit",               "单位" },
    { "alarmlow",           "报警下限" },
    { "alarmhigh",          "报警上限" },
    { "archiveenabled",     "是否归档" },
    { "archiveintervalsec", "归档周期" },
    { "archiveintervalsec", "归档间隔" },
};

// 把一个表头单元格文本映射到规范列名；不是已知列名则返回空串。
QString canonicalColumnName(const QString &cell)
{
    const QString key = cell.trimmed().toLower();
    if (key.isEmpty())
        return QString();
    for (const auto &a : kColumnAliases) {
        // 中文别名是 UTF-8 字节，必须用 fromUtf8 转换（QLatin1String 会逐字节按
        // Latin-1 解释，导致中文表头永远匹配不上）。
        if (key == QString::fromUtf8(a.alias))
            return QString::fromLatin1(a.canonical);
    }
    return QString();
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
    // 列顺序与 defaultOrder 严格一致；新增列一律追加到末尾以保持旧文件兼容。
    out << "id,name,serverAddress,registerType,address,count,dataType,scale,offset,unit,alarmLow,alarmHigh,archiveEnabled,archiveIntervalSec,byteOrder\n";
    for (const auto &p : m_points) {
        QStringList cells;
        cells << QString::number(p.id)
              << csvEscape(p.name)
              << QString::number(p.serverAddress)
              << QString::number(static_cast<int>(p.registerType))
              << QString::number(p.address)
              << QString::number(p.count)
              << csvEscape(p.dataType)
              // 'g' + 17 位有效数字：保证导出再导入后数值无损（默认 6 位会截断）
              << QString::number(p.scale, 'g', 17)
              << QString::number(p.offset, 'g', 17)
              << csvEscape(p.unit)
              << QString::number(p.alarmLow, 'g', 17)
              << QString::number(p.alarmHigh, 'g', 17)
              << QString(p.archiveEnabled ? QLatin1String("1") : QLatin1String("0"))
              << QString::number(p.archiveIntervalSec)
              << csvEscape(DataParser::byteOrderToToken(p.byteOrder));
        out << cells.join(QLatin1Char(',')) << '\n';
    }
    out.flush();
    return out.status() == QTextStream::Ok;
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
    QStringList invalidReasons;
    int skipped = 0;
    int row = 0;
    for (const auto &v : doc.array()) {
        ++row;
        const QJsonObject obj = v.toObject();
        PointDefinition p = pointFromJson(obj);
        if (p.address < 0 || p.address > 65535) {
            invalidReasons << QStringLiteral("第 %1 项：地址 %2 非法（应为 0~65535），已跳过").arg(row).arg(p.address);
            ++skipped;
            continue;
        }
        if (p.count < 1)
            p.count = 1;
        if (p.scale == 0.0) {
            invalidReasons << QStringLiteral("第 %1 项：系数为 0，已重置为 1.0").arg(row);
            p.scale = 1.0;
        }
        parsed.append(p);
    }

    if (parsed.isEmpty()) {
        if (imported) *imported = 0;
        if (error) {
            *error = QStringLiteral("未解析到有效点位（共 %1 项被跳过）。").arg(skipped);
            if (!invalidReasons.isEmpty())
                *error += QStringLiteral("\n") + invalidReasons.mid(0, 10).join(QStringLiteral("\n"));
        }
        return false;
    }

    if (!append) {
        m_points.clear();
        m_values.clear();
        m_nextId = 1;   // 覆盖导入时重置 id 分配器，避免 id 跳号
    }
    for (const PointDefinition &p : parsed)
        addPoint(p);
    if (imported) *imported = parsed.size();
    if (error && !invalidReasons.isEmpty()) {
        *error = QStringLiteral("成功导入 %1 个点位；%2 项被修正/跳过：\n%3")
                     .arg(parsed.size()).arg(skipped)
                     .arg(invalidReasons.mid(0, 10).join(QStringLiteral("\n")));
    }
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

    // 表头识别：首行中「≥2 个单元格是已知列名」才认定为表头。
    // 之前是「任一单元格命中关键字即认定表头」—— 无表头 CSV 首行恰好叫 "name"
    // 就会被当表头丢弃（丢一个点位）。
    QHash<QString, int> col;
    bool hasHeader = false;
    const QStringList headerRow = rows.first();
    {
        int knownCount = 0;
        QHash<QString, int> headerCols;
        for (int i = 0; i < headerRow.size(); ++i) {
            const QString canonical = canonicalColumnName(headerRow.at(i));
            if (canonical.isEmpty())
                continue;
            ++knownCount;
            // 同一规范列名出现多次时保留第一次出现的位置
            if (!headerCols.contains(canonical))
                headerCols.insert(canonical, i);
        }
        if (knownCount >= 2) {
            hasHeader = true;
            col = headerCols;
        }
    }

    // 无表头时按导出顺序定位（仅在确实没有表头时使用，不能在有表头时盲猜下标 ——
    // 否则表头只含部分列时，回退填入的下标会与已有列撞车，字段被映射到错误的列）。
    // 注意顺序必须与 exportToCsv 的输出列顺序严格一致，且新增列只能追加到末尾，
    // 否则旧的无表头 CSV 会整体错位（这是导出格式的向后兼容约束）。
    const QStringList defaultOrder = { "id", "name", "serveraddress", "registertype", "address",
                                       "count", "datatype", "scale", "offset", "unit",
                                       "alarmlow", "alarmhigh", "archiveenabled", "archiveintervalsec",
                                       "byteorder" };
    if (!hasHeader) {
        for (int i = 0; i < defaultOrder.size(); ++i)
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
    QStringList invalidReasons;
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

        // ---- 从站地址 ----
        {
            bool ok = false;
            const QString txt = cellOf(row, QStringLiteral("serveraddress"));
            const int v = txt.isEmpty() ? 1 : txt.toInt(&ok);
            if (!txt.isEmpty() && !ok) {
                invalidReasons << QStringLiteral("第 %1 行：从站地址 '%2' 非法").arg(r + 1).arg(txt);
                ++skipped;
                continue;
            }
            p.serverAddress = (v <= 0 || v > 247) ? 1 : v;
        }

        // ---- 寄存器类型 ----
        {
            QModbusDataUnit::RegisterType type = QModbusDataUnit::HoldingRegisters;
            if (registerTypeFromToken(cellOf(row, QStringLiteral("registertype")), &type))
                p.registerType = type;
        }

        // ---- 起始地址（严格校验：非法地址的点位永远匹配不到任何响应） ----
        {
            bool ok = false;
            const int v = addrText.toInt(&ok);
            if (!addrText.isEmpty() && (!ok || v < 0 || v > 65535)) {
                invalidReasons << QStringLiteral("第 %1 行：地址 '%2' 非法（应为 0~65535）").arg(r + 1).arg(addrText);
                ++skipped;
                continue;
            }
            p.address = v;
        }

        // ---- 寄存器数量 ----
        {
            bool ok = false;
            const QString txt = cellOf(row, QStringLiteral("count"));
            const int v = txt.isEmpty() ? 1 : txt.toInt(&ok);
            if (!txt.isEmpty() && !ok) {
                invalidReasons << QStringLiteral("第 %1 行：数量 '%2' 非法").arg(r + 1).arg(txt);
                ++skipped;
                continue;
            }
            p.count = qBound(1, v, 4);
        }

        // ---- 数据类型 ----
        {
            const QString dt = cellOf(row, QStringLiteral("datatype"));
            p.dataType = dt.isEmpty() ? QStringLiteral("uint16") : dt;
        }

        // ---- 字节序 ----
        {
            const QString tok = cellOf(row, QStringLiteral("byteorder"));
            if (!tok.isEmpty()) {
                bool ok = false;
                const ByteOrder bo = DataParser::byteOrderFromToken(tok, &ok);
                if (!ok) {
                    invalidReasons << QStringLiteral("第 %1 行：字节序 '%2' 无法识别，按 ABCD 处理").arg(r + 1).arg(tok);
                }
                p.byteOrder = bo;
            }
        }

        // ---- 系数 / 偏移（非法数值必须报错，不能静默变 0） ----
        {
            const QString scaleText = cellOf(row, QStringLiteral("scale"));
            if (scaleText.isEmpty()) {
                p.scale = 1.0;
            } else {
                bool ok = false;
                const double v = scaleText.toDouble(&ok);
                if (!ok) {
                    invalidReasons << QStringLiteral("第 %1 行：系数 '%2' 非法，按 1.0 处理").arg(r + 1).arg(scaleText);
                    p.scale = 1.0;
                } else {
                    p.scale = v;
                }
            }
            const QString offsetText = cellOf(row, QStringLiteral("offset"));
            if (offsetText.isEmpty()) {
                p.offset = 0.0;
            } else {
                bool ok = false;
                const double v = offsetText.toDouble(&ok);
                if (!ok) {
                    invalidReasons << QStringLiteral("第 %1 行：偏移 '%2' 非法，按 0.0 处理").arg(r + 1).arg(offsetText);
                    p.offset = 0.0;
                } else {
                    p.offset = v;
                }
            }
        }

        p.unit = cellOf(row, QStringLiteral("unit"));

        // ---- 报警阈值 ----
        {
            const QString lowText = cellOf(row, QStringLiteral("alarmlow"));
            bool ok = false;
            const double low = lowText.isEmpty() ? 0.0 : lowText.toDouble(&ok);
            if (!lowText.isEmpty() && !ok) {
                invalidReasons << QStringLiteral("第 %1 行：报警下限 '%2' 非法，按 0.0 处理").arg(r + 1).arg(lowText);
            }
            p.alarmLow = lowText.isEmpty() ? 0.0 : (ok ? low : 0.0);

            const QString highText = cellOf(row, QStringLiteral("alarmhigh"));
            bool ok2 = false;
            const double high = highText.isEmpty() ? 65535.0 : highText.toDouble(&ok2);
            if (!highText.isEmpty() && !ok2) {
                invalidReasons << QStringLiteral("第 %1 行：报警上限 '%2' 非法，按 65535 处理").arg(r + 1).arg(highText);
            }
            p.alarmHigh = highText.isEmpty() ? 65535.0 : (ok2 ? high : 65535.0);
            if (p.alarmHigh <= p.alarmLow)
                p.alarmHigh = 65535.0;
        }

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
        if (error) {
            *error = QStringLiteral("未解析到有效点位（共 %1 行被跳过），请检查表头或列顺序。").arg(skipped);
            if (!invalidReasons.isEmpty())
                *error += QStringLiteral("\n") + invalidReasons.mid(0, 10).join(QStringLiteral("\n"));
        }
        return false;
    }

    if (!append) {
        m_points.clear();
        m_values.clear();
        m_nextId = 1;   // 覆盖导入时重置 id 分配器，避免 id 跳号
    }
    for (const PointDefinition &p : parsed)
        addPoint(p);

    if (imported) *imported = parsed.size();
    // 部分行非法时导入仍然成功（好行已导入），但必须把被跳过的行与原因报告给用户，
    // 否则用户以为全部导入成功，实际部分点位静默丢失。
    if (error && !invalidReasons.isEmpty()) {
        *error = QStringLiteral("成功导入 %1 个点位；%2 行被跳过：\n%3")
                     .arg(parsed.size()).arg(skipped)
                     .arg(invalidReasons.mid(0, 10).join(QStringLiteral("\n")));
    }
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
