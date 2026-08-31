
#include "writeregistermodel.h"

// 列索引定义
enum { NumColumn = 0, CoilsColumn = 1, HoldingColumn = 2, ColumnCount = 3 };

// 最大地址空间（0~65535）
static const int kMaxAddressSpace = 65536;

WriteRegisterModel::WriteRegisterModel(QObject *parent)
    : QAbstractTableModel(parent)
    , m_number(10)
    , m_address(0)
    , m_coils(kMaxAddressSpace, false)
    , m_holdingRegisters(kMaxAddressSpace, 0u)
{
}

int WriteRegisterModel::rowCount(const QModelIndex &/*parent*/) const
{
    // 行数由用户选择的数量决定
    return m_number;
}

int WriteRegisterModel::columnCount(const QModelIndex &/*parent*/) const
{
    return ColumnCount;
}

QVariant WriteRegisterModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_number || index.column() >= ColumnCount)
        return QVariant();

    // 模型中每一行对应绝对地址 m_address + index.row()
    const int absoluteAddress = m_address + index.row();
    if (absoluteAddress < 0 || absoluteAddress >= kMaxAddressSpace)
        return QVariant();

    if (index.column() == NumColumn && role == Qt::DisplayRole)
        return QString::number(index.row());

    if (index.column() == CoilsColumn && role == Qt::CheckStateRole) // coils
        return m_coils.testBit(absoluteAddress) ? Qt::Checked : Qt::Unchecked;

    if (index.column() == HoldingColumn && role == Qt::DisplayRole) // holding registers
        return QString("0x%1").arg(QString::number(m_holdingRegisters.at(absoluteAddress), 16));

    return QVariant();
}

QVariant WriteRegisterModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Horizontal) {
        switch (section) {
        case NumColumn:
            return QStringLiteral("#");
        case CoilsColumn:
            return QStringLiteral("Coils  ");
        case HoldingColumn:
            return QStringLiteral("Holding Registers");
        default:
            break;
        }
    }
    return QVariant();
}

bool WriteRegisterModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_number || index.column() >= ColumnCount)
        return false;

    const int absoluteAddress = m_address + index.row();
    if (absoluteAddress < 0 || absoluteAddress >= kMaxAddressSpace)
        return false;

    if (index.column() == CoilsColumn && role == Qt::CheckStateRole) { // coils
        auto s = static_cast<Qt::CheckState>(value.toUInt());
        s == Qt::Checked ? m_coils.setBit(absoluteAddress) : m_coils.clearBit(absoluteAddress);
        emit dataChanged(index, index);
        return true;
    }

    if (index.column() == HoldingColumn && role == Qt::EditRole) { // holding registers
        bool result = false;
        quint16 newValue = value.toString().toUShort(&result, 16);
        if (result)
            m_holdingRegisters[absoluteAddress] = newValue;

        emit dataChanged(index, index);
        return result;
    }

    return false;
}

Qt::ItemFlags WriteRegisterModel::flags(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_number || index.column() >= ColumnCount)
        return QAbstractTableModel::flags(index);

    Qt::ItemFlags flags = QAbstractTableModel::flags(index);

    if (index.column() == CoilsColumn) // coils
        return flags | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == HoldingColumn) // holding registers
        return flags | Qt::ItemIsEditable | Qt::ItemIsEnabled | Qt::ItemIsSelectable;

    return flags | Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void WriteRegisterModel::setStartAddress(int address)
{
    // 记录新的起始地址并刷新视图
    m_address = address;
    emit updateViewport();
}

void WriteRegisterModel::setNumberOfValues(const QString &number)
{
    // 更新行数并通知视图变更
    bool ok = false;
    int newNumber = number.toInt(&ok);
    if (!ok) return;

    if (newNumber < 0) newNumber = 0;
    if (newNumber > kMaxAddressSpace) newNumber = kMaxAddressSpace;

    if (m_number == newNumber) {
        emit updateViewport();
        return;
    }

    beginResetModel();
    m_number = newNumber;
    endResetModel();
    emit updateViewport();
}
