#ifndef WRITEGUARD_H
#define WRITEGUARD_H

// writeguard.h
// 点位级写保护 / 联锁的统一判定层。
//
// 所有真正下发 Modbus 写请求的入口（手动写、读写、批量写、远程 API）都必须先经过
// WriteGuard::check()，避免"某个入口忘了判断"导致保护形同虚设。
//
// 判定只依赖点位定义与当前值，不依赖 GUI / Modbus，便于单元测试。

#include <QModbusDataUnit>
#include <QString>
#include <QVector>

#include "pointmodel.h"

struct WriteGuardResult {
    bool allowed = true;
    bool blockedByProtection = false;   // 命中写保护
    bool blockedByInterlock = false;    // 命中联锁条件
    // 已本地化的拒绝原因（可直接显示到状态栏 / 日志 / API 响应）。
    QString reason;
    // 与本次写区间重叠的点位 id（无论允许与否都返回，供审计记录）。
    QVector<int> pointIds;
};

class WriteGuard
{
public:
    // 点位实际占用的寄存器/位数量：count 与数据类型所需长度取较大值。
    // （用户可能把 count 配成 1 但类型是 float32，此时仍占 2 个寄存器。）
    static int registerSpan(const PointDefinition &point);

    // 写区间是否与点位重叠。设备/从站/寄存器类型任一不同都不算重叠。
    static bool overlaps(const PointDefinition &point, int deviceId, int serverAddress,
                         QModbusDataUnit::RegisterType type, int startAddress, int count);

    // 统一判定。
    //   points / values 通常是 PointModel::points() 与 currentValues()。
    //   deviceId 0 = 主连接；>0 = DeviceManager 设备。
    // 返回的 reason 已翻译（QCoreApplication::translate），测试在无 translator 时
    // 得到英文源串。
    static WriteGuardResult check(const QVector<PointDefinition> &points,
                                  const QVector<PointValue> &values,
                                  int deviceId, int serverAddress,
                                  QModbusDataUnit::RegisterType type,
                                  int startAddress, int count);

    // 数据质量英文名（用于拼拒绝原因）。
    static QString qualityName(DataQuality quality);
};

#endif // WRITEGUARD_H
