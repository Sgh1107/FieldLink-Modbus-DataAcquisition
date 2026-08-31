
#ifndef WRITEREGISTERMODEL_H
#define WRITEREGISTERMODEL_H

#include <QAbstractItemModel>
#include <QBitArray>
#include <QObject>

class WriteRegisterModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    WriteRegisterModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

    Qt::ItemFlags flags(const QModelIndex &index) const override;

public slots:
    void setStartAddress(int address);
    void setNumberOfValues(const QString &number);

signals:
    void updateViewport();

public:
    // 当前写操作的数量（行数）与起始地址
    int m_number;
    int m_address;

    // 全地址空间缓存（绝对地址索引）。
    // 为了简化实现，直接分配最大 65536 地址空间，内存占用可接受。
    QBitArray m_coils;                // 线圈表缓存（true/false）
    QVector<quint16> m_holdingRegisters; // 保持寄存器缓存（16 位）
};

#endif // WRITEREGISTERMODEL_H
