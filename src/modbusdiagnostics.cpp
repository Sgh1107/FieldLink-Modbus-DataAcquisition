#include "modbusdiagnostics.h"

#include <QRegularExpression>

namespace ModbusDiagnostics {

// ==================== 十六进制 PDU 解析 ====================

bool parseHexBytes(const QString &text, QByteArray *out, QString *error)
{
    if (out)
        out->clear();
    if (!out)
        return false;

    QString s = text;
    // 允许空格、逗号、分号、冒号、下划线、连字符作为分隔符
    s.remove(QRegularExpression(QStringLiteral("[\\s,;:_-]")));
    // 允许 0x / 0X 前缀（可出现在任意位置，不只是开头）
    s.remove(QRegularExpression(QStringLiteral("0[xX]")));

    if (s.isEmpty())
        return true;   // 允许空 PDU：部分功能码（FC07/FC11 等）无数据域

    if (s.size() % 2 != 0) {
        if (error)
            *error = QStringLiteral("十六进制字符数必须为偶数（每字节 2 个字符），当前 %1 个")
                         .arg(s.size());
        return false;
    }

    for (int i = 0; i < s.size(); i += 2) {
        const QString chunk = s.mid(i, 2);
        bool ok = false;
        const int b = chunk.toInt(&ok, 16);
        if (!ok) {
            if (error)
                *error = QStringLiteral("非法十六进制片段：%1").arg(chunk);
            return false;
        }
        out->append(static_cast<char>(b & 0xFF));
    }
    return true;
}

QString hexByte(quint8 value)
{
    return QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
}

QString hexBytes(const QByteArray &bytes)
{
    QStringList parts;
    parts.reserve(bytes.size());
    for (int i = 0; i < bytes.size(); ++i)
        parts << hexByte(static_cast<quint8>(bytes.at(i)));
    return parts.join(QLatin1Char(' '));
}

// ==================== 异常码 ====================

QString exceptionName(int exceptionCode)
{
    switch (exceptionCode) {
    case 0x01: return QStringLiteral("非法功能");
    case 0x02: return QStringLiteral("非法数据地址");
    case 0x03: return QStringLiteral("非法数据值");
    case 0x04: return QStringLiteral("从站设备故障");
    case 0x05: return QStringLiteral("确认(处理中)");
    case 0x06: return QStringLiteral("从站设备忙");
    case 0x08: return QStringLiteral("存储奇偶校验错");
    case 0x0A: return QStringLiteral("网关路径不可用");
    case 0x0B: return QStringLiteral("网关目标设备响应失败");
    default:   return QStringLiteral("未知异常");
    }
}

// ==================== 原始报文构造 ====================

QByteArray readPayload(int startAddress, int count)
{
    // Modbus 规范：多字节字段一律大端（高字节在前）
    QByteArray d;
    d.append(static_cast<char>((startAddress >> 8) & 0xFF));
    d.append(static_cast<char>(startAddress & 0xFF));
    d.append(static_cast<char>((count >> 8) & 0xFF));
    d.append(static_cast<char>(count & 0xFF));
    return d;
}

bool isValidFunctionCode(int code)
{
    // 0 保留；1..0x7F 为合法功能码；0x80+ 用于标识异常响应，不是请求功能码
    return code >= 0x01 && code <= 0x7F;
}

// ==================== 扫描器逻辑 ====================

static const ProbeOption kProbeOptions[] = {
    { 0x03, QModbusDataUnit::HoldingRegisters, "03 读保持寄存器" },
    { 0x01, QModbusDataUnit::Coils,            "01 读线圈" },
    { 0x04, QModbusDataUnit::InputRegisters,   "04 读输入寄存器" },
    { 0x02, QModbusDataUnit::DiscreteInputs,   "02 读离散输入" },
};

const ProbeOption *probeOptions()
{
    return kProbeOptions;
}

int probeOptionCount()
{
    return static_cast<int>(sizeof(kProbeOptions) / sizeof(kProbeOptions[0]));
}

QModbusDataUnit::RegisterType registerTypeForFunctionCode(int functionCode)
{
    for (int i = 0; i < probeOptionCount(); ++i) {
        if (kProbeOptions[i].code == functionCode)
            return kProbeOptions[i].type;
    }
    return QModbusDataUnit::HoldingRegisters;   // 未知功能码按保持寄存器处理（与界面默认一致）
}

bool validateScanRange(int startUnit, int endUnit, int *totalOut, QString *error)
{
    if (totalOut)
        *totalOut = 0;

    // 0 为广播地址，不能用于扫描（广播请求从站不响应）
    if (startUnit < 1 || startUnit > 247) {
        if (error)
            *error = QStringLiteral("起始地址必须在 1~247 之间（0 为广播地址，不可扫描）。");
        return false;
    }
    if (endUnit < 1 || endUnit > 247) {
        if (error)
            *error = QStringLiteral("结束地址必须在 1~247 之间（0 为广播地址，不可扫描）。");
        return false;
    }
    if (endUnit < startUnit) {
        if (error)
            *error = QStringLiteral("结束地址必须不小于起始地址。");
        return false;
    }

    if (totalOut)
        *totalOut = endUnit - startUnit + 1;
    return true;
}

// ==================== 轮询任务生成 ====================

PollTaskSeed makePollTaskSeed(int unit, int functionCode, int startAddress, int count)
{
    PollTaskSeed seed;
    seed.id = 40000 + unit;   // 稳定可重现：同一从站重复生成会覆盖同一条任务
    seed.name = QStringLiteral("扫描生成-从站%1").arg(unit);
    seed.serverAddress = unit;
    seed.registerType = registerTypeForFunctionCode(functionCode);
    seed.startAddress = qMax(0, startAddress);
    seed.quantity = qMax(1, count);      // 与 PollManager 语义一致：数量至少为 1
    seed.intervalMs = 1000;
    seed.alarmEnabled = false;
    return seed;
}

// ==================== 统计文本 ====================

QString scanProgressText(int scanned, int total, int found)
{
    return QStringLiteral("已扫描 %1/%2，发现 %3 个在线从站")
        .arg(scanned).arg(total).arg(qMax(0, found));
}

} // namespace ModbusDiagnostics