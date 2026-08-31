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
    static QString toAsciiString(const QVector<quint16> &registers, ByteOrder order = ByteOrder::BigEndian_ABCD);

    static QVector<quint16> fromFloat32(float value, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static QVector<quint16> fromInt32(qint32 value, ByteOrder order = ByteOrder::BigEndian_ABCD);
    static QVector<quint16> fromAsciiString(const QString &str, ByteOrder order = ByteOrder::BigEndian_ABCD);

    static quint16 swapBytes(quint16 value);
    static void reorderBytes(quint8 *data, int size, ByteOrder order);

    static QString byteOrderName(ByteOrder order);
    static QStringList byteOrderNames();
};

#endif // DATAPARSER_H
