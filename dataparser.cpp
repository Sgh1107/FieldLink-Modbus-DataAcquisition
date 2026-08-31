#include "dataparser.h"
#include <QtEndian>
#include <cstring>
#include <algorithm>

DataParser::DataParser(QObject *parent) : QObject(parent) {}

void DataParser::reorderBytes(quint8 *data, int size, ByteOrder order)
{
    if (size == 4) {
        quint8 tmp[4];
        memcpy(tmp, data, 4);
        switch (order) {
        case ByteOrder::BigEndian_ABCD:
            break;
        case ByteOrder::LittleEndian_DCBA:
            data[0] = tmp[3]; data[1] = tmp[2]; data[2] = tmp[1]; data[3] = tmp[0];
            break;
        case ByteOrder::MidBigEndian_BADC:
            data[0] = tmp[1]; data[1] = tmp[0]; data[2] = tmp[3]; data[3] = tmp[2];
            break;
        case ByteOrder::MidLittleEndian_CDAB:
            data[0] = tmp[2]; data[1] = tmp[3]; data[2] = tmp[0]; data[3] = tmp[1];
            break;
        }
    }
}

float DataParser::toFloat32(quint16 regHi, quint16 regLo, ByteOrder order)
{
    quint8 bytes[4];
    bytes[0] = (regHi >> 8) & 0xFF;
    bytes[1] = regHi & 0xFF;
    bytes[2] = (regLo >> 8) & 0xFF;
    bytes[3] = regLo & 0xFF;
    reorderBytes(bytes, 4, order);
    float result;
    memcpy(&result, bytes, 4);
    return result;
}

double DataParser::toFloat64(quint16 r0, quint16 r1, quint16 r2, quint16 r3, ByteOrder order)
{
    quint8 bytes[8];
    bytes[0] = (r0 >> 8) & 0xFF; bytes[1] = r0 & 0xFF;
    bytes[2] = (r1 >> 8) & 0xFF; bytes[3] = r1 & 0xFF;
    bytes[4] = (r2 >> 8) & 0xFF; bytes[5] = r2 & 0xFF;
    bytes[6] = (r3 >> 8) & 0xFF; bytes[7] = r3 & 0xFF;
    Q_UNUSED(order);
    double result;
    memcpy(&result, bytes, 8);
    return result;
}

qint32 DataParser::toInt32(quint16 regHi, quint16 regLo, ByteOrder order)
{
    quint8 bytes[4];
    bytes[0] = (regHi >> 8) & 0xFF;
    bytes[1] = regHi & 0xFF;
    bytes[2] = (regLo >> 8) & 0xFF;
    bytes[3] = regLo & 0xFF;
    reorderBytes(bytes, 4, order);
    qint32 result;
    memcpy(&result, bytes, 4);
    return result;
}

quint32 DataParser::toUInt32(quint16 regHi, quint16 regLo, ByteOrder order)
{
    quint8 bytes[4];
    bytes[0] = (regHi >> 8) & 0xFF;
    bytes[1] = regHi & 0xFF;
    bytes[2] = (regLo >> 8) & 0xFF;
    bytes[3] = regLo & 0xFF;
    reorderBytes(bytes, 4, order);
    quint32 result;
    memcpy(&result, bytes, 4);
    return result;
}

QString DataParser::toAsciiString(const QVector<quint16> &registers, ByteOrder order)
{
    QString result;
    for (quint16 reg : registers) {
        quint8 hi = (reg >> 8) & 0xFF;
        quint8 lo = reg & 0xFF;
        if (order == ByteOrder::LittleEndian_DCBA || order == ByteOrder::MidBigEndian_BADC) {
            std::swap(hi, lo);
        }
        if (hi >= 0x20 && hi <= 0x7E) result += QChar(hi);
        if (lo >= 0x20 && lo <= 0x7E) result += QChar(lo);
    }
    return result;
}

QVector<quint16> DataParser::fromFloat32(float value, ByteOrder order)
{
    quint8 bytes[4];
    memcpy(bytes, &value, 4);
    reorderBytes(bytes, 4, order);
    quint16 regHi = (quint16(bytes[0]) << 8) | bytes[1];
    quint16 regLo = (quint16(bytes[2]) << 8) | bytes[3];
    return {regHi, regLo};
}

QVector<quint16> DataParser::fromInt32(qint32 value, ByteOrder order)
{
    quint8 bytes[4];
    memcpy(bytes, &value, 4);
    reorderBytes(bytes, 4, order);
    quint16 regHi = (quint16(bytes[0]) << 8) | bytes[1];
    quint16 regLo = (quint16(bytes[2]) << 8) | bytes[3];
    return {regHi, regLo};
}

QVector<quint16> DataParser::fromAsciiString(const QString &str, ByteOrder order)
{
    QVector<quint16> result;
    QByteArray ascii = str.toLatin1();
    for (int i = 0; i < ascii.size(); i += 2) {
        quint8 hi = ascii.at(i);
        quint8 lo = (i + 1 < ascii.size()) ? ascii.at(i + 1) : 0;
        if (order == ByteOrder::LittleEndian_DCBA || order == ByteOrder::MidBigEndian_BADC) {
            std::swap(hi, lo);
        }
        result.append((quint16(hi) << 8) | lo);
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
