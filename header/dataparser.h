#ifndef DATAPARSER_H
#define DATAPARSER_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QStringList>
#include <QtEndian>

enum class ByteOrder {
    BigEndian_ABCD = 0,
    LittleEndian_DCBA,
    MidBigEndian_BADC,
    MidLittleEndian_CDAB
};

class DataParser : public QObject
{
    Q_OBJECT

public:
    explicit DataParser(QObject *parent = nullptr);

    static float toFloat32(quint16 regHi, quint16 regLo, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static double toFloat64(quint16 r0, quint16 r1, quint16 r2, quint16 r3, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static qint32 toInt32(quint16 regHi, quint16 regLo, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static quint32 toUInt32(quint16 regHi, quint16 regLo, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static quint64 toUInt64(quint16 r0, quint16 r1, quint16 r2, quint16 r3, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static QString toAsciiString(const QVector<quint16> &registers, ByteOrder order = ByteOrder::BigEndian_ABCD);

    // 按 Modbus wire 顺序（寄存器内高字节在前）还原原始字节串。
    // 与 toAsciiString 的区别：不套用数值字节序 —— 字节序描述的是多寄存器数值的
    // 分组方式，对 8 位字符流没有意义；且本函数不做字符过滤，字节原样保留。
    static QByteArray toByteString(const QVector<quint16> &registers);

    static QVector<quint16> fromFloat32(float value, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static QVector<quint16> fromFloat64(double value, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static QVector<quint16> fromInt32(qint32 value, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static QVector<quint16> fromUInt32(quint32 value, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static QVector<quint16> fromAsciiString(const QString &str, ByteOrder order = ByteOrder::BigEndian_ABCD);

    static quint16 swapBytes(quint16 value);
    static void reorderBytes(quint8 *data, int size, ByteOrder order);

    static QString byteOrderName(ByteOrder order);
    static QStringList byteOrderNames();

    // 字节序文本 <-> 枚举 的双向映射（"ABCD"/"DCBA"/"BADC"/"CDAB"）。
    // 供设备模板、点表 CSV/JSON 导入导出共用，避免各处重复实现导致不一致。
    static ByteOrder byteOrderFromToken(const QString &token, bool *ok = nullptr);
    static QString byteOrderToToken(ByteOrder order);
};

#endif // DATAPARSER_H
