#ifndef MODBUSDIAGNOSTICS_H
#define MODBUSDIAGNOSTICS_H

// Modbus 调试工具的纯逻辑层（无 GUI、无网络依赖，可被单元测试直接覆盖）
//
// 抽出这些函数的目的：
//   1. 「原始功能码窗」的十六进制解析 / 异常码解释 / 字节格式化，
//      以及「总线扫描器」的探测报文构造 / 功能码映射 / 区间校验 / 轮询任务生成，
//      原本都是匿名 namespace 里的静态函数或槽函数内部逻辑，无法独立测试；
//   2. 抽出后既能被两个对话框复用，也能被 tests/ 覆盖，避免逻辑只能靠点界面验证。
//
// 本文件不得包含任何 QtWidgets / QDialog 头，保持「纯逻辑、可无 GUI 编译」。

#include <QByteArray>
#include <QString>
#include <QModbusDataUnit>

namespace ModbusDiagnostics {

// ---------- 十六进制 PDU 解析 ----------

// 把 "0x00 0x0A" / "00,0a" / "000A" / "00_0A" 等文本解析为字节数组。
// - 空文本视为「合法且为空」（部分功能码无数据域，如 FC07/FC11）。
// - 奇数长度、非法十六进制字符 → 返回 false 并给出可读错误信息。
bool parseHexBytes(const QString &text, QByteArray *out, QString *error);

// 字节 → 两位大写十六进制文本（"0A" / "FF"），用于日志与表格显示。
QString hexByte(quint8 value);

// 字节数组 → "0A 0B 0C" 形式的大写十六进制文本。
QString hexBytes(const QByteArray &bytes);

// ---------- 异常码 ----------

// Modbus 异常码 → 中文说明。未知码返回 "未知异常"（不返回空串，便于日志可读）。
QString exceptionName(int exceptionCode);

// ---------- 原始报文构造 ----------

// 读类请求的数据域：起始地址(2 字节大端) + 数量(2 字节大端)。
// Modbus 规范要求所有多字节字段为大端（高字节在前）。
QByteArray readPayload(int startAddress, int count);

// 原始报文完整性校验：功能码必须在 1..0x7F（0 保留；0x80+ 用于异常响应）。
bool isValidFunctionCode(int code);

// ---------- 扫描器逻辑 ----------

// 扫描区间校验。start/end 应落在 1..247（0 为广播，不用于扫描）。
// 校验失败时给出原因，成功时输出总探测地址数（含首尾）。
bool validateScanRange(int startUnit, int endUnit, int *totalOut, QString *error);

// 扫描器可选的探测功能码（与界面下拉一致）。
struct ProbeOption {
    int code = 0;                                        // 功能码
    QModbusDataUnit::RegisterType type = QModbusDataUnit::HoldingRegisters;  // 对应寄存器表
    const char *label = nullptr;
};
const ProbeOption *probeOptions();
int probeOptionCount();

// 探测功能码 → 寄存器表类型（供「生成轮询任务」复用）。
// 未知功能码按 HoldingRegisters 处理（与界面默认一致）。
QModbusDataUnit::RegisterType registerTypeForFunctionCode(int functionCode);

// ---------- 轮询任务生成 ----------

// 由扫描结果生成轮询任务的固定参数。
struct PollTaskSeed {
    int id = 0;              // 任务 id
    QString name;            // 任务名
    int serverAddress = 1;   // 从站地址
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::HoldingRegisters;
    int startAddress = 0;
    int quantity = 1;
    int intervalMs = 1000;
    bool alarmEnabled = false;
};

// 为「扫描发现的某个从站」构造一个轮询任务种子（尚未填入 PollManager）。
// - unit 应为 1..247；
// - 任务 id 采用 40000 + unit，便于识别来源且稳定可重现（重复生成会覆盖同一条）。
// - name 形如「扫描生成-从站12」。
PollTaskSeed makePollTaskSeed(int unit, int functionCode, int startAddress, int count);

// ---------- 统计文本 ----------

// 「已扫描 %1/%2，发现 %3 个在线从站」——抽出来便于测试文案与数值裁剪。
QString scanProgressText(int scanned, int total, int found);

} // namespace ModbusDiagnostics

#endif // MODBUSDIAGNOSTICS_H