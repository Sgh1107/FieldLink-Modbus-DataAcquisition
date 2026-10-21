#include "dataparser.h"
#include <QtEndian>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// 字节序语义（与 Modbus 探针/主流组态约定一致）：
//   32 位值按大端字节序记为 A(最高有效字节) B C D(最低有效字节)。
//   两个寄存器 (regHi, regLo) 的四个字节在 wire 上按序记为
//   r0h, r0l, r1h, r1l，各字节序的 wire→值 映射：
//
//     ABCD (Big-Endian)     : A,B,C,D  →  r0h=A r0l=B r1h=C r1l=D
//     DCBA (Little-Endian)  : D,C,B,A  →  r0h=D r0l=C r1h=B r1l=A
//     BADC (Mid-Big)        : B,A,D,C  →  r0h=B r0l=A r1h=D r1l=C
//     CDAB (Mid-Little)     : C,D,A,B  →  r0h=C r0l=D r1h=A r1l=B
//
//   64 位（r0..r3 共 8 字节 H0h,H0l,H1h,H1l,H2h,H2l,H3h,H3l）按同样模式扩展。
// 旧实现把"主机内存字节序"与"寄存器 wire 字节序"混用，标准编码解析错误（见 P1）。
// ---------------------------------------------------------------------------

namespace {

// wire 寄存器字节 → 32 位值（大端位序）
quint32 rawFromRegisters32(quint16 regHi, quint16 regLo, ByteOrder order)
{
    const quint32 r0h = (regHi >> 8) & 0xFF;
    const quint32 r0l = regHi & 0xFF;
    const quint32 r1h = (regLo >> 8) & 0xFF;
    const quint32 r1l = regLo & 0xFF;
    switch (order) {
    case ByteOrder::BigEndian_ABCD:       return (r0h << 24) | (r0l << 16) | (r1h << 8) | r1l;
    case ByteOrder::LittleEndian_DCBA:    return (r1l << 24) | (r1h << 16) | (r0l << 8) | r0h;
    case ByteOrder::MidBigEndian_BADC:    return (r0l << 24) | (r0h << 16) | (r1l << 8) | r1h;
    case ByteOrder::MidLittleEndian_CDAB: return (r1h << 24) | (r1l << 16) | (r0h << 8) | r0l;
    }
    return (r0h << 24) | (r0l << 16) | (r1h << 8) | r1l;
}

// 32 位值（大端位序）→ 寄存器（rawFromRegisters32 的逆映射）
void registersFromRaw32(quint32 raw, ByteOrder order, quint16 &regHi, quint16 &regLo)
{
    const quint32 v0 = (raw >> 24) & 0xFF;   // 最高有效字节
    const quint32 v1 = (raw >> 16) & 0xFF;
    const quint32 v2 = (raw >> 8) & 0xFF;
    const quint32 v3 = raw & 0xFF;           // 最低有效字节
    switch (order) {
    case ByteOrder::BigEndian_ABCD:       regHi = static_cast<quint16>((v0 << 8) | v1); regLo = static_cast<quint16>((v2 << 8) | v3); break;
    case ByteOrder::LittleEndian_DCBA:    regHi = static_cast<quint16>((v3 << 8) | v2); regLo = static_cast<quint16>((v1 << 8) | v0); break;
    case ByteOrder::MidBigEndian_BADC:    regHi = static_cast<quint16>((v1 << 8) | v0); regLo = static_cast<quint16>((v3 << 8) | v2); break;
    case ByteOrder::MidLittleEndian_CDAB: regHi = static_cast<quint16>((v2 << 8) | v3); regLo = static_cast<quint16>((v0 << 8) | v1); break;
    }
}

// 8 字节 wire 序 → 64 位值
quint64 rawFromRegisters64(const quint8 wire[8], ByteOrder order)
{
    quint64 raw = 0;
    switch (order) {
    case ByteOrder::BigEndian_ABCD:
        for (int i = 0; i < 8; ++i) raw = (raw << 8) | wire[i];
        break;
    case ByteOrder::LittleEndian_DCBA:
        for (int i = 0; i < 8; ++i) raw = (raw << 8) | wire[7 - i];
        break;
    case ByteOrder::MidBigEndian_BADC:   // 每寄存器字节交换，寄存器顺序不变
        for (int reg = 0; reg < 4; ++reg) {
            raw = (raw << 8) | wire[reg * 2 + 1];
            raw = (raw << 8) | wire[reg * 2];
        }
        break;
    case ByteOrder::MidLittleEndian_CDAB: // 寄存器两两交换，寄存器内字节不变
    {
        // wire = [V2,V3,V0,V1,V6,V7,V4,V5]
        const int valueIndex[8] = { 2, 3, 0, 1, 6, 7, 4, 5 };
        for (int i = 0; i < 8; ++i)
            raw = (raw << 8) | wire[valueIndex[i]];
        break;
    }
    }
    return raw;
}

// 64 位值 → 8 字节 wire 序（rawFromRegisters64 的逆映射）
void registersFromRaw64(quint64 raw, ByteOrder order, quint8 wire[8])
{
    for (int i = 0; i < 8; ++i)
        wire[i] = static_cast<quint8>((raw >> (56 - 8 * i)) & 0xFF);   // 大端序列 V0..V7

    auto swapBytesInPlace = [](quint8 &a, quint8 &b) { const quint8 t = a; a = b; b = t; };
    switch (order) {
    case ByteOrder::BigEndian_ABCD:
        break;
    case ByteOrder::LittleEndian_DCBA:
        std::reverse(wire, wire + 8);
        break;
    case ByteOrder::MidBigEndian_BADC:
        swapBytesInPlace(wire[0], wire[1]); swapBytesInPlace(wire[2], wire[3]);
        swapBytesInPlace(wire[4], wire[5]); swapBytesInPlace(wire[6], wire[7]);
        break;
    case ByteOrder::MidLittleEndian_CDAB:
        swapBytesInPlace(wire[0], wire[2]); swapBytesInPlace(wire[1], wire[3]);
        swapBytesInPlace(wire[4], wire[6]); swapBytesInPlace(wire[5], wire[7]);
        break;
    }
}

} // namespace

DataParser::DataParser(QObject *parent) : QObject(parent) {}

// 兼容保留：对 4 字节缓冲做字节序重排（A,B,C,D 为值字节的大端序号）。
// 旧语义按主机内存序解释，已弃用；外部无调用方，仅为 ABI 兼容保留。
void DataParser::reorderBytes(quint8 *data, int size, ByteOrder order)
{
    if (size != 4)
        return;
    quint8 wire[4] = { data[0], data[1], data[2], data[3] };
    quint32 raw = 0;
    switch (order) {
    case ByteOrder::BigEndian_ABCD:       raw = (quint32(wire[0]) << 24) | (quint32(wire[1]) << 16) | (quint32(wire[2]) << 8) | wire[3]; break;
    case ByteOrder::LittleEndian_DCBA:    raw = (quint32(wire[3]) << 24) | (quint32(wire[2]) << 16) | (quint32(wire[1]) << 8) | wire[0]; break;
    case ByteOrder::MidBigEndian_BADC:    raw = (quint32(wire[1]) << 24) | (quint32(wire[0]) << 16) | (quint32(wire[3]) << 8) | wire[2]; break;
    case ByteOrder::MidLittleEndian_CDAB: raw = (quint32(wire[2]) << 24) | (quint32(wire[3]) << 16) | (quint32(wire[0]) << 8) | wire[1]; break;
    }
    data[0] = static_cast<quint8>((raw >> 24) & 0xFF);
    data[1] = static_cast<quint8>((raw >> 16) & 0xFF);
    data[2] = static_cast<quint8>((raw >> 8) & 0xFF);
    data[3] = static_cast<quint8>(raw & 0xFF);
}

float DataParser::toFloat32(quint16 regHi, quint16 regLo, ByteOrder order)
{
    const quint32 raw = rawFromRegisters32(regHi, regLo, order);
    float result;
    memcpy(&result, &raw, 4);   // IEEE754：32 位值位模式即主机表示
    return result;
}

double DataParser::toFloat64(quint16 r0, quint16 r1, quint16 r2, quint16 r3, ByteOrder order)
{
    const quint8 wire[8] = {
        static_cast<quint8>((r0 >> 8) & 0xFF), static_cast<quint8>(r0 & 0xFF),
        static_cast<quint8>((r1 >> 8) & 0xFF), static_cast<quint8>(r1 & 0xFF),
        static_cast<quint8>((r2 >> 8) & 0xFF), static_cast<quint8>(r2 & 0xFF),
        static_cast<quint8>((r3 >> 8) & 0xFF), static_cast<quint8>(r3 & 0xFF)
    };
    const quint64 raw = rawFromRegisters64(wire, order);
    double result;
    memcpy(&result, &raw, 8);
    return result;
}

qint32 DataParser::toInt32(quint16 regHi, quint16 regLo, ByteOrder order)
{
    const quint32 raw = rawFromRegisters32(regHi, regLo, order);
    qint32 result;
    memcpy(&result, &raw, 4);
    return result;
}

quint32 DataParser::toUInt32(quint16 regHi, quint16 regLo, ByteOrder order)
{
    return rawFromRegisters32(regHi, regLo, order);
}

quint64 DataParser::toUInt64(quint16 r0, quint16 r1, quint16 r2, quint16 r3, ByteOrder order)
{
    const quint8 wire[8] = {
        static_cast<quint8>((r0 >> 8) & 0xFF), static_cast<quint8>(r0 & 0xFF),
        static_cast<quint8>((r1 >> 8) & 0xFF), static_cast<quint8>(r1 & 0xFF),
        static_cast<quint8>((r2 >> 8) & 0xFF), static_cast<quint8>(r2 & 0xFF),
        static_cast<quint8>((r3 >> 8) & 0xFF), static_cast<quint8>(r3 & 0xFF)
    };
    return rawFromRegisters64(wire, order);
}

QVector<quint16> DataParser::fromUInt32(quint32 value, ByteOrder order)
{
    quint32 raw;
    memcpy(&raw, &value, 4);
    quint16 regHi = 0, regLo = 0;
    registersFromRaw32(raw, order, regHi, regLo);
    return {regHi, regLo};
}

// 按 Modbus wire 顺序还原原始字节：每个寄存器高字节在前。
// 不套用数值字节序（字节序描述的是多寄存器数值的分组方式，对字节流无意义），
// 也不做字符过滤 —— 字节原样保留，由调用方决定如何解释。
QByteArray DataParser::toByteString(const QVector<quint16> &registers)
{
    QByteArray out;
    out.reserve(registers.size() * 2);
    for (quint16 reg : registers) {
        out.append(static_cast<char>((reg >> 8) & 0xFF));
        out.append(static_cast<char>(reg & 0xFF));
    }
    return out;
}

QString DataParser::toAsciiString(const QVector<quint16> &registers, ByteOrder order)
{
    // 字节序描述的是"多寄存器数值在 wire 上的分组方式"，对 8 位字符流没有意义。
    // 之前在这里套用 DCBA/BADC 会交换寄存器内的高低字节（"AB" → "BA"），
    // 并且只保留 0x20~0x7E 的可打印 ASCII，其余字节被无声丢弃
    // （设备寄存器里存 UTF-8 中文时整个字符串会变成空串）。
    // 现在按 Modbus wire 顺序（寄存器内高字节在前）逐字节还原，
    // 非 ASCII 字节保留为 Latin-1 字符（原始字节不丢）。
    Q_UNUSED(order);
    QString result;
    for (quint16 reg : registers) {
        const quint8 hi = (reg >> 8) & 0xFF;
        const quint8 lo = reg & 0xFF;
        result += QChar(hi);
        result += QChar(lo);
    }
    return result;
}

QVector<quint16> DataParser::fromFloat32(float value, ByteOrder order)
{
    quint32 raw;
    memcpy(&raw, &value, 4);   // 主机位模式 = IEEE754 值位
    quint16 regHi = 0, regLo = 0;
    registersFromRaw32(raw, order, regHi, regLo);
    return {regHi, regLo};
}

QVector<quint16> DataParser::fromFloat64(double value, ByteOrder order)
{
    quint64 bits;
    memcpy(&bits, &value, 8);   // 主机位模式 = IEEE754 值位
    quint8 wire[8];
    registersFromRaw64(bits, order, wire);
    quint16 r0 = static_cast<quint16>((wire[0] << 8) | wire[1]);
    quint16 r1 = static_cast<quint16>((wire[2] << 8) | wire[3]);
    quint16 r2 = static_cast<quint16>((wire[4] << 8) | wire[5]);
    quint16 r3 = static_cast<quint16>((wire[6] << 8) | wire[7]);
    return {r0, r1, r2, r3};
}

QVector<quint16> DataParser::fromInt32(qint32 value, ByteOrder order)
{
    quint32 raw;
    memcpy(&raw, &value, 4);
    quint16 regHi = 0, regLo = 0;
    registersFromRaw32(raw, order, regHi, regLo);
    return {regHi, regLo};
}

QVector<quint16> DataParser::fromAsciiString(const QString &str, ByteOrder order)
{
    QVector<quint16> result;
    // 与 toAsciiString 对称：字节序对 8 位字符流没有意义，不交换字节内高低位。
    // 之前这里对 DCBA/BADC 交换，而 toAsciiString 也交换 —— 两者同时交换时
    // 往返恰好自洽，但与真实设备按 wire 顺序存放的字符串不一致。
    // 现在两边都不交换：始终按 Modbus wire 顺序（寄存器内高字节在前）。
    Q_UNUSED(order);
    QByteArray ascii = str.toLatin1();
    for (int i = 0; i < ascii.size(); i += 2) {
        quint8 hi = static_cast<quint8>(ascii.at(i));
        quint8 lo = (i + 1 < ascii.size()) ? static_cast<quint8>(ascii.at(i + 1)) : 0;
        result.append(static_cast<quint16>((hi << 8) | lo));
    }
    return result;
}

quint16 DataParser::swapBytes(quint16 value)
{
    return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF);
}

QString DataParser::byteOrderName(ByteOrder order)
{
    switch (order) {
    case ByteOrder::BigEndian_ABCD: return "Big-Endian (ABCD)";
    case ByteOrder::LittleEndian_DCBA: return "Little-Endian (DCBA)";
    case ByteOrder::MidBigEndian_BADC: return "Mid-Big-Endian (BADC)";
    case ByteOrder::MidLittleEndian_CDAB: return "Mid-Little-Endian (CDAB)";
    }
    return "Unknown";
}

QStringList DataParser::byteOrderNames()
{
    return {
        byteOrderName(ByteOrder::BigEndian_ABCD),
        byteOrderName(ByteOrder::LittleEndian_DCBA),
        byteOrderName(ByteOrder::MidBigEndian_BADC),
        byteOrderName(ByteOrder::MidLittleEndian_CDAB)
    };
}

// 字节序文本 → 枚举。
// 接受 "ABCD"/"DCBA"/"BADC"/"CDAB"（不区分大小写），以及 byteOrderName() 里的完整文案。
// 未知文本返回 false（调用方应回退到 ABCD 并给出提示），
// 不能像 registerType 那样用 contains() 模糊匹配 —— "CDAB" 与 "DCBA" 互不为子串，
// 但宽松匹配容易把拼写错误静默变成另一种字节序（数值错误且无提示）。
ByteOrder DataParser::byteOrderFromToken(const QString &token, bool *ok)
{
    const QString t = token.trimmed().toUpper();
    if (ok) *ok = true;
    if (t.isEmpty())
        return ByteOrder::BigEndian_ABCD;
    if (t == "ABCD" || t.contains("BIG-ENDIAN") || t.contains("大端"))
        return ByteOrder::BigEndian_ABCD;
    if (t == "DCBA" || t.contains("LITTLE-ENDIAN") || t.contains("小端"))
        return ByteOrder::LittleEndian_DCBA;
    if (t == "BADC" || t.contains("MID-BIG-ENDIAN"))
        return ByteOrder::MidBigEndian_BADC;
    if (t == "CDAB" || t.contains("MID-LITTLE-ENDIAN"))
        return ByteOrder::MidLittleEndian_CDAB;
    if (ok) *ok = false;
    return ByteOrder::BigEndian_ABCD;
}

QString DataParser::byteOrderToToken(ByteOrder order)
{
    switch (order) {
    case ByteOrder::BigEndian_ABCD: return QStringLiteral("ABCD");
    case ByteOrder::LittleEndian_DCBA: return QStringLiteral("DCBA");
    case ByteOrder::MidBigEndian_BADC: return QStringLiteral("BADC");
    case ByteOrder::MidLittleEndian_CDAB: return QStringLiteral("CDAB");
    }
    return QStringLiteral("ABCD");
}
