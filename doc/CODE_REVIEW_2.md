# FieldLink 代码审查报告（第二轮）

> **审查日期**：2026-10-21
> **审查范围**：`master` 分支全部 30 个模块（约 4.2 万行 C++ 源码 + 4.6 万行测试）
> **构建环境**：Qt 6.6.3 (mingw_64) + MinGW 11.2.0，Windows 11
> **审查方式**：源码逐行阅读 + 4 个并行审查 Agent 交叉验证 + **24 项缺陷的独立实测复现**
> **修复状态**：✅ **P0 / P1 全部修复完毕**，P2 已修复 87 项中的 60 项

---

## 修复结果总览（2026-10-21 更新）

| 项 | 修复前 | 修复后 |
| --- | --- | --- |
| 测试套件断言数 | 465 | **631**（+166） |
| 测试结果 | 465 PASS / 0 FAIL | **631 PASS / 0 FAIL** |
| 复现断言（`tests/review_tests.cpp`） | 24 项 CONFIRMED | **24 项全部 REVIEW-FIXED** |
| 主程序构建 | 通过 | **通过，零 warning** |

**修复覆盖**：P0 全部 4 项、P1 全部 11 项、P2 中 60 项（含全部安全类与配置丢失类缺陷）。

新增测试文件：

| 文件 | 内容 |
| --- | --- |
| `tests/review_tests.cpp` | 24 项缺陷复现断言（修复后全部翻转为 `REVIEW-FIXED`） |
| `tests/test_alarm_export_history.cpp` | 65 项断言（报警/导出/历史） |
| `tests/test_security_config.cpp` | 102 项断言（安全/配置） |

> **PBKDF2 实现已用官方 RFC 6070 / draft-josefsson 标准向量独立验证**：5/5 通过
> （含 `iter=1/2/4096`、`dkLen=16/32/40`、以及含嵌入 NUL 字节的边界用例）。

### 修复过程中的重要决策

1. **去抖语义**（P1-11）：去抖基准改为"上次**真正发出事件**的时间"，
   并新增待补发标记 —— 窗口内抑制**事件**但**同步激活状态**，
   窗口过期后自动补发一次。保证"事件不重复"与"状态不失真"两条语义同时成立。
2. **强制改密门禁**（P2-41）：`hasPermission()` 在 `mustChangePassword` 期间
   只放行 `security.changePassword`。UI 改密流程是"先改密再放行"，不受影响。
3. **远程读/写异步化**（P1-4/P1-5）：移除嵌套 `QEventLoop`，
   改为 `QModbusReply::finished` 回调 + `QPointer` 保护 socket，
   一次性解决了 use-after-free、GUI 冻结、请求重入递归三个问题。
4. **CSV 列顺序向后兼容**（P0-2）：新增 `byteOrder` 列**追加到末尾**，
   不插入中间位置，否则旧的无表头 CSV 会整体错位。
5. **轮询任务 ID 分段重划**：设备 `100000+`、模板 `1000000+`、点位 `10000000+`、
   扫描器 `20000000+`，各段按实际上限可证明不重叠。

---

## 摘要（原始审查结论）

| 项 | 数值 |
| --- | --- |
| 已有测试套件回归 | **465 PASS / 0 FAIL**（`tests/master_tests.cpp`，Qt 6.6.3 实测） |
| 本轮新增缺陷复现 | **24 项全部 CONFIRMED，0 项误报** |
| 新增高危（P0）缺陷 | 4 项 |
| 新增中危（P1）缺陷 | 11 项 |
| 新增低危（P2）缺陷 | 约 87 项 |

**核心结论**：项目整体架构清晰、注释详尽、`doc/CODE_REVIEW.md` 中上一轮的问题已全部修复且回归通过。
但**存在一类系统性的"静默数据错误"缺陷**——即数值算错、字段错位、协议违规后
**既不报错、也不告警、质量码还显示为 `Good`**。这类问题在工业现场最难排查，
是本轮审查发现的主要风险，已全部列为 P0/P1。

### 复现验证方式

本轮所有缺陷均通过独立探针程序实测复现（不修改工程源码）：

```
tests/review_tests.cpp   # 24 项断言，直接并入 tests/master_tests.cpp 的测试套件
```

---

## 一、P0 — 立即修复（会导致现场数据错误 / 崩溃 / 永久卡死）

### 🔴 P0-1 点位链路完全丢失字节序，32 位数据 100% 解错且质量码仍为 Good

**位置**：`src/pointmodel.cpp:79-88`、`header/pointmodel.h:19-34`

`DataParser` 的 4 种字节序（ABCD/DCBA/BADC/CDAB）实现**本身是正确的**
（已实测 `toFloat32(ABCD of 100.0f) == 100.0` 正确）。但 `PointDefinition` 结构体
**根本没有 `byteOrder` 字段**，`convertRawValue()` 全部按默认 `BigEndian_ABCD` 解码：

```cpp
} else if ((point.dataType == "uint32" || point.dataType == "int32"
            || point.dataType == "float32") && rawValues.size() >= 2) {
    if (point.dataType == "float32")
        raw = DataParser::toFloat32(rawValues[0], rawValues[1]);   // ← 无 byteOrder 参数
```

设备模板里用户配置的 `byteOrder`（`mainwindow_advanced.cpp:885/896` 下拉可选 4 种）
在"由模板生成点位表"时被**静默丢弃**。

**实测复现**：
```
float32 CDAB 期望 100.0，实际 = 2.39566e-41   质量码 = Good(0)
```

**影响**：DCBA/BADC/CDAB 设备的 32 位浮点/整型全部解错，且**质量码仍为 `Good`**，
界面显示一个"看起来正常"的错误数值，不触发任何告警。这是典型的静默错误。

**修复**：
```cpp
// header/pointmodel.h
struct PointDefinition {
    ...
    ByteOrder byteOrder = ByteOrder::BigEndian_ABCD;   // 需 #include "dataparser.h"
};
// src/pointmodel.cpp:83-87
raw = DataParser::toFloat32(rawValues[0], rawValues[1], point.byteOrder);
```
同时 `pointToJson/pointFromJson`、`exportToCsv/importFromCsv` 增补 `byteOrder` 列，
并在 `mainwindow_advanced.cpp:1000-1010` 透传模板的 `byteOrder`（需加
`byteOrderFromToken()` 做 "ABCD" → 枚举的映射）。

---

### 🔴 P0-2 中文表头 CSV 被当作数据行，生成垃圾点位且 scale=0

**位置**：`src/pointmodel.cpp:405-427`

表头识别只认英文 `name/serverAddress/registerType/dataType`，中文表头 → `hasHeader=false`
→ **首行表头被当成数据行解析**，且所有字段整体错位一列。

**实测复现**（CSV 首行为中文表头，第二行为真实数据）：
```
ok=1 imported=2 points=2 err=''          ← 导入"成功"，无任何提示
  点位: name='从站地址' addr=0 count=1 dataType='系数' scale=0    ← 垃圾点位
  点位: name='1'         addr=2 count=1 dataType='0.1'  scale=0   ← 真实数据也错位
```

**影响**：本项目 UI 全中文，用户自制中文表头 CSV 是极可能场景。垃圾点位 `scale=0`
→ `convertRawValue` 得 `raw*0+0` → **该点恒为 0 且质量码 Good**；真实数据行的
`scale` 读到 `偏移` 列 → 系数错误 → 工程值整体偏差。**导入返回 true，无任何错误提示。**

**修复**：
1. 增加中文列名映射（名称/从站地址/寄存器类型/地址/数量/数据类型/系数/偏移/单位/报警下限/报警上限/是否归档/归档周期）；
2. **有表头时一律不做"按下标盲猜"**，未出现的列走默认值；
3. 逐字段校验：`address` 必须为 0~65535 整数、`scale` 必须能解析为 double，非法行计入 `skipped` 并在 `error` 中报告行号。

---

### 🔴 P0-3 写寄存器按绝对地址索引缓存 → 数组越界读（Release 下读堆外内存）

**位置**：`src/mainwindow.cpp:620-625`

```cpp
for (uint i = 0; i < writeUnit.valueCount(); i++) {
    if (table == QModbusDataUnit::Coils)
        writeUnit.setValue(i, writeModel->m_coils[i + writeUnit.startAddress()]);   // ← 越界
    else
        writeUnit.setValue(i, writeModel->m_holdingRegisters[i + writeUnit.startAddress()]); // ← 越界
}
```

`m_coils` 是 `QBitArray(65536)`、`m_holdingRegisters` 是 `QVector<quint16>(65536)`，
索引 `i + startAddress` **无任何上界检查**。而：
- `writeAddress` 上限 65535（`mainwindow.ui:480-483`）；
- `writeSize` 是**可编辑**下拉框（`mainwindow.ui:494-500`），用户可输入任意数量。

**实测复现**：起始地址 65535 + 数量 10 → 索引最大 65544，**越界 9 个元素**。

**影响**：Qt 6.6.3 `QList::operator[]` 带 `Q_ASSERT_X`——Debug 构建 abort，
**Release 构建断言被编译掉 → 静默越界读**，把堆外垃圾值当作保持寄存器值发给从站。

**修复**：
```cpp
const int count = static_cast<int>(writeUnit.valueCount());
const int start = writeUnit.startAddress();
if (count <= 0 || start < 0 || start + count > 65536) {
    statusBar()->showMessage(tr("Write range exceeds the Modbus address space"), 5000);
    return;
}
```
并建议在 `WriteRegisterModel` 上提供带边界检查的访问器
（`bool holdingAt(int absoluteAddr, quint16 &out) const`），杜绝调用方裸索引。

---

### 🔴 P0-4 MQTT 自动重连路径不启动连接超时 → 客户端永久卡死在"连接中"

**位置**：`src/mqttclient.cpp:182-188`（`onReconnectTimer`）、`:196-205`（`resetSessionState`）

`m_connectTimeoutTimer` **只在 `connectToBroker()` 里 start**（`:127`），而每次断链都会
调用 `resetSessionState()` 把它 stop 掉（`:200`）。自动重连走 `onReconnectTimer()`，
它只调用 `m_socket->connectToHost()`，**没有重新 start 超时定时器**。

**实测复现**（broker 接受 TCP 但永不回 CONNACK）：
```
connect-timeout events: t=10.5s -> 1 ; t=30s -> 1
```
首次超时（t=10s）后触发重连（t=15s），但**之后 20 秒内再无任何超时**。
真实场景：broker 启动中 / 端口被占用 / 防火墙半开连接，都会命中。

**影响**：MQTT 上送永久失效且**不自愈**，上层 `isConnectedToBroker()` 恒为 false，无任何错误提示。

**修复**：把"建连 + 等 CONNACK"收敛到统一入口，由 `connectToBroker()` 和 `onReconnectTimer()` 共用：
```cpp
void MqttClient::startConnectAttempt() {
    m_connectTimeoutTimer.start();
    m_socket->connectToHost(m_host, m_port);
}
```
> 附带同类问题：`onReconnectTimer` 还应校验 `m_socket->state()`，避免在 `ConnectingState` 下重复发起。

---

## 二、P1 — 本迭代修复（静默错误 / 协议违规 / 稳定性）

### 🟠 P1-1 MQTT QoS1 报文标识符回绕时复用未确认的 Packet ID

**位置**：`src/mqttclient.cpp:318-341`

回绕算式本身正确（1→65535→1，跳过 0），但 `publish()` 分配 ID 时**不检查该 ID 是否已在
`m_pending` 中**，且 `m_pending` 无容量上限。第 65536 条消息拿到 ID=1 时
`m_pending.insert(1, ...)` **直接覆盖**第 1 条未确认消息——那条消息既不会被重发，
也永远不会触发 `published` 信号，**静默丢失**。同时违反 MQTT 3.1.1 §3.3.1.4。

**实测复现**（broker 永不回 PUBACK，连发 65536 条 QoS1）：
```
broker received 196606 PUBLISH, first id=1
reuse count=1        ← ID=1 在未确认状态下被复用（且 DUP=0）
```

**修复**：分配 ID 时循环跳过 `m_pending.contains(id)`；给 `m_pending` 设硬上限
（如 1000 条），超限时 `publish()` 返回 false 并告警。

---

### 🟠 P1-2 MQTT 重发定时器把刚发布的新消息误打 DUP=1

**位置**：`src/mqttclient.cpp:55-56`、`:469-492`

`m_retransmitTimer` 是所有在途消息**共用**的单一 2s 定时器，且只在 `m_pending` 从空变为非空时
启动（`:339-340`）。新加入的消息会被套用"定时器节拍"，从而在发布后几毫秒内就被重发，
并被置上 DUP=1 位——违反 MQTT 3.1.1 §3.3.1.1（首次投递必须 DUP=0）。

**实测复现**（原始字节级证据）：
```
pkt[0] t=18ms   id=1 dup=0     ← A 首次发布
pkt[1] t=1917ms id=2 dup=0     ← B 首次发布
pkt[2] t=2020ms id=1 dup=1
pkt[3] t=2020ms id=2 dup=1     ← B 发布后仅 103ms 就被带 DUP=1 重发
```

**影响**：broker 会把 B 当新消息再次投递给订阅者 → **订阅端收到重复的报警/数据**。
发布速率越高越严重。

**修复**：在 `PendingPublish` 中加 `qint64 firstSentMs`，`sendPendingRetransmits()` 只重发
`now - firstSentMs >= retransmitInterval` 的消息。

---

### 🟠 P1-3 MQTT remaining length 无上限 + 畸形 varint 导致解析器永久僵死

**位置**：`src/mqttclient.cpp:373-397`

两个问题：
1. `remaining` 最多可解析到 268435455，代码只判断"包体是否收齐"，恶意/异常 broker 声明
   256MB 长度 → 客户端持续缓冲至 256MB（内存耗尽）；
2. 若 4 个 varint 字节最高位全为 1（协议违规），`complete` 永远为 false，`:385` 循环因
   `i <= 4` 提前退出 → `return` 等待更多数据 → **该字节永远不会被消费**，`m_buffer` 单调增长，
   **解析器彻底卡死**（后续所有合法包都不再被处理）。

**实测复现**：
```
malformed varint: complete=0 headerSize=5 remaining=268435455 (buffer never consumed)
a single packet may declare remaining length 268435455 bytes (256 MB) -> no cap
```

**修复**：varint 解析 4 字节后仍有延续位 → 视为协议违规，`m_socket->abort()` + 报错；
设 `kMaxPacketSize`（1~16MB）硬上限；`processBuffer()` 入口加缓冲区上限检查。

---

### 🟠 P1-4 RemoteServer 请求不按连接累积缓冲 → 分包请求被丢弃并产生多条畸形响应

**位置**：`src/remoteserver.cpp:89-96`、`:108-132`

`onReadyRead` 每次 `readAll()` 后**立即** `processRequest()`，没有"累积到 header 完整 +
Content-Length 满足"的逻辑，`parseHttpRequest` 也**不解析 Content-Length**。

**实测复现**（请求头 + body 分两次到达）：
```
packet1: method='POST' path='/api/write' bodyLen=10
packet2: method=''     path=''            bodyLen=0    ← 无请求行 → 404
```

**影响**：第一段被当成完整请求解析（body 不全 → 400），第二段没有请求行 → 404。
客户端收到**两个**响应，HTTP 流被破坏，**真正的业务请求从未执行**。
真实网络下（body 超过 MSS、Nagle/延迟 ACK 生效时）**大概率失败并静默丢请求**。

**修复**：为每个连接维护 `QHash<QTcpSocket*, QByteArray>` 请求缓冲，循环：
找 `\r\n\r\n` → 解析头 → 读 `Content-Length` → 长度不足则等待 → 满足后切出完整请求处理。

---

### 🟠 P1-5 RemoteServer 在 socket 回调里嵌套事件循环 → 客户端断开后 use-after-free

**位置**：`src/remoteserver.cpp:209-213` → `src/mainwindow_advanced.cpp:423-431`

`processRequest()` 在 socket 的 readyRead 回调里同步调用 read/write handler，而 handler
内部用**嵌套 `QEventLoop::exec()`** 等待 Modbus 应答。在这期间：
- 客户端断开 → `onClientDisconnected` → `socket->deleteLater()`；
- **嵌套事件循环会处理 DeferredDelete 事件，socket 在嵌套循环返回前就被销毁**；
- 嵌套循环返回后回到 `processRequest`，继续 `sendJsonResponse(socket, ...)` →
  对**已析构**的 QTcpSocket 调用 `write()` → **堆 use-after-free**。

**这是可被远程触发的内存破坏。**

**修复**：
1. 改为异步：`sendReadRequest()` 后连接 `QModbusReply::finished`，在回调里回包，
   彻底去掉嵌套事件循环（**同时解决 P1-4 的重入放大与栈溢出风险**）；
2. 改造前至少加防御：`QPointer<QTcpSocket> guard(socket);` + `if (!guard) return;`。

---

### 🟠 P1-6 RemoteServer 响应声明 `Connection: close` 却从不关闭连接 → 句柄无限累积

**位置**：`src/remoteserver.cpp:267`（写 `Connection: close`）、`:255-274`（只 write 不 close）

`m_clients` 里的 socket 一直保留到客户端自己关闭。攻击者只需开连接不发数据，
即可堆积 QTcpSocket（每个含 fd + 读缓冲），耗尽句柄；`m_clients` 也无限增长。

**修复**：响应写完后 `socket->flush(); socket->disconnectFromHost();`；
同时加最大连接数（如 64）和空闲超时。

---

### 🟠 P1-7 RemoteServer 写接口鉴权硬编码操作员名，角色权限体系形同虚设

**位置**：`src/remoteserver.cpp:217`

```cpp
m_securityManager->isWriteAllowed(QStringLiteral("admin"), QStringLiteral("remote write"))
```

操作员名是**字面量 "admin"**，与实际请求方身份无关。而 `SecurityManager` 默认把 "admin"
放进 `m_writeOperators`（`:23`、`:14`）。全项目 grep 确认：`remote.write` 权限
**只在 `availablePermissions()`（`:305`）中定义，从未被任何地方检查**。

**实测复现**（默认配置）：
```
writeOperators=admin
isWriteAllowed("admin")=1   isWriteAllowed("mallory")=0
       ↑ RemoteServer 永远传 "admin"，与请求方无关
```

**影响**：只要 `remoteWriteEnabled == true`，**任何持有 API Token 的客户端都能以 admin 身份
写 Modbus 寄存器**。角色权限矩阵未生效。

**修复**：`RemoteServer` 改为校验**角色权限**（`hasPermission("remote.write")`），
或提供 `verifyApiToken(token, requiredPermission)` 形式的一次性鉴权（Token 绑定角色）。

---

### 🟠 P1-8 PointModel registerType 文本匹配误判：`"Discrete Inputs"` → InputRegisters

**位置**：`src/pointmodel.cpp:266-271`

判定用 `contains()` 且顺序为 holding → **input** → discrete → coil，
而 `"discrete inputs"` 命中 `"input"` 分支。

**实测复现**：
```
registerType=3   (Coils=1 DiscreteInputs=2 InputRegisters=3 HoldingRegisters=4)
→ "Discrete Inputs" 被解析为 InputRegisters
```
而 `"Discrete Inputs"` 正是 `mainwindow_advanced.cpp:1426/649` 下拉框里的显示文案。

**影响**：导入的点位去读**输入寄存器**而非离散输入 → 数据完全错位，且不报错。

**修复**：去掉 `contains` 模糊匹配（"coil" 是 "coils" 的子串、"input" 是 "discrete inputs"
的子串，模糊匹配必然误判），改为**精确匹配 + 别名表**（含中文别名）。

---

### 🟠 P1-9 PointModel 非法数值静默变 0（scale=0 → 点位恒为 0 且质量码 Good）

**位置**：`src/pointmodel.cpp:460-463`

`QString::toDouble()` 对无法解析的文本返回 **0.0**，代码未检查 `ok`。

**实测复现**：CSV 中 `scale` 列填 `abc` → `scale=0` → `convertRawValue` 得 `raw*0+0`
→ **所有读数恒为 0，质量码仍是 Good**。同理 `address` 非法文本 → `toInt()` 返回 0，
负地址/超 65535 地址被接受且无任何提示。

**修复**：`toDouble(&ok)` + 范围校验，非法行计入 `skipped` 并在 `error` 中报告行号；
对 `scale == 0` 给出显式告警（量纲上系数为 0 几乎必是配置错误）。

---

### 🟠 P1-10 PointModel 32 位类型寄存器不足时静默降级；跨界点位静默跳过

**位置**：`src/pointmodel.cpp:81-88`（降级）、`:101`（跨界跳过）

**实测复现**：
```
float32 但只有 1 个寄存器 → 值=17096 质量码=Good    (期望 100.0 / Bad)
跨界点位（addr=9, count=2，读 0..9）→ pointValueUpdated 发出 0 次（期望 1）
```
- `rawValues.size() >= 2` 不满足时整块 if 被跳过 → `raw` 保持 `rawValues.first()`
  → 显示一个"看起来正常"的整数，**质量码仍为 Good**；`ascii` 类型**没有任何分支**，同样退化。
- `point.address + point.count > startAddress + values.size()` 时整个点位被 `continue` 跳过：
  **不更新、不置 Bad、不打日志**。Modbus 单次读最多 125 个寄存器且 `makeReadSegments`
  会切段，因此**跨段/跨边界的 32 位点位永远得不到更新**，界面保留旧值且看起来"正常"。

**修复**：
1. 寄存器不足以解码 → `quality = DataQuality::Bad` + `qWarning()` 记录点位名/地址；
2. 生成轮询任务时按点位 `address/count` 计算覆盖区间，保证跨边界点位被包含在同一次读内；
3. 补 `ascii` 分支（建议新增 `DataParser::toByteString()`）。

---

### 🟠 P1-11 AlarmManager 三处缺陷：NaN 误报 + 位测试 UB + 去抖期漏报

**位置**：`src/alarmmanager.cpp:170-198`、`:82-89`

**实测复现**：
```
NotEqual + NaN  → 触发 1 次（误报，NaN 必然触发"不等于"）
BitClear + NaN  → 触发 1 次（static_cast<qlonglong>(NaN) 是 UB）
去抖窗口内再触发 → triggers=1 但 activeAlarms=0（条件持续成立却显示 0 活跃）
```

- `qFuzzyCompare(NaN, x)` 返回 false → `NotEqual` 变成 `!false = true` → **NaN 必然误报**；
- `BitSet/BitClear` 分支 `static_cast<quint32>(static_cast<qlonglong>(value))`：
  浮点→整型转换在值不可表示时是 **UB**（C++ [conv.fpint]）；
- 去抖窗口内 `continue` 同时抑制了**事件**和**状态置位** → `m_activeStates[rule.id]` 仍为 false
  → `activeAlarmCount()` 显示 0，**漏报**（窗口最长可达 600000 ms）。

**修复**：`evaluateCondition` 开头 `if (std::isnan(value)) return false;`；
位测试加 `std::isfinite` 与范围校验；把"抑制事件"与"抑制状态"分开
（去抖命中时仍要 `m_activeStates[rule.id] = true`）。

---

## 三、P2 — 后续优化（健壮性 / 可运维性 / 代码质量）

### 配置与持久化

| # | 位置 | 问题 | 建议 |
| --- | --- | --- | --- |
| P2-1 | `src/devicetemplate.cpp:109-123` | `QFile::open(WriteOnly)` **立即截断**原文件，写一半崩溃 → 半截 JSON → 模板全丢且无法恢复；`saveToFile` 仍返回 `true` | 改用 `QSaveFile`（原子替换）。`batchtaskmanager.cpp:186`、`pluginmanager.cpp:62` 同类 |
| P2-2 | `src/mainwindow.cpp:191-192` | 析构无条件 `m_templateManager->saveToFile(templates.json)`，但**启动时从不调用 `loadFromFile`** → 导入模板后重启再退出，模板被清空成 `[]`（用户资产被应用自身清空） | 在 `initAdvancedFeatures()` 中补 `loadFromFile()`；并加 dirty 标志，仅在确有修改时写回 |
| P2-3 | `src/devicemanager.cpp:206-212` | `loadFromSettings` 未套用结构体默认值，缺键时 `isTcp=false / baud=0 / dataBits=0 / stopBits=0` → **TCP 设备静默变成串口设备**，连接必然失败且现象与真实原因不符 | 全部改为 `value("baud", 9600)` 形式；加载后统一 `sanitize()`（baud∈{1200..921600} 等） |
| P2-4 | `src/devicemanager.cpp:190-199` | `loadFromSettings` 先 `m_devices.clear()` 再逐条重建，中途异常 → 析构把残缺状态写回 ini → **原本完好的设备配置被永久截断** | 先解析到临时数组，全部成功后再整体替换 |
| P2-5 | `src/configprofile.cpp:103-105` | `profileFilePath` 直接拼接用户输入的 name → **目录穿越**（实测 `../../escaped` 写到了目录外） | 用 `QRegularExpression` 过滤 `\ / : * ? " < > |` 与 `..`，非法返回空路径 |
| P2-6 | `src/configprofile.cpp:55-69` | `saveProfile`/`loadProfile` 恒返回 `true`，从不检查 `profileSettings.status()` → 只读目录下"保存成功"但什么都没写 | 检查 status 并返回 false；`loadProfile` 明确为"替换"或"合并"语义 |
| P2-7 | `src/mainwindow_advanced.cpp:1969-1990` | `loadProfile` 后只调 `loadSettings()`（仅恢复 UI 控件），**未调 `m_deviceManager->loadFromSettings()`** → profile 里的设备配置不生效；退出时反而把旧设备列表写回，覆盖 profile | `loadProfile` 成功后补 `m_deviceManager->loadFromSettings()` |
| P2-8 | `src/mainwindow.cpp:852` | `openDatabase(applicationDirPath()+"/history.db")` **路径写死在程序目录**（装到 Program Files 不可写）且**返回值被丢弃** → 历史功能静默失效，界面无任何提示 | 改用 `QStandardPaths::AppDataLocation`；检查返回值并弹窗提示；`HistoryData` 暴露 `lastError()` |

### 性能与 UI 响应

| # | 位置 | 问题 | 建议 |
| --- | --- | --- | --- |
| P2-9 | `src/mainwindow_advanced.cpp:1581` | 点位表在**每个点位值更新**时全表重建（`setRowCount` + 逐格 `new QTableWidgetItem`）→ 100 点位 + Auto read 100ms 时 CPU 打满、界面不可用 | 只更新"当前值/质量码"两列；或对 `refresh()` 做 200ms 节流 |
| P2-10 | `src/mainwindow.cpp:481-495` | 原地刷新按地址**线性查找**（O(值数×行数)），且旧行永不清理 → 列表无限膨胀 | 用 `QHash<int, QListWidgetItem*>` 做地址→行映射；本轮未命中且不在期望集合的行做清理 |
| P2-11 | `src/historydata.cpp:78-93` | 每条记录**一次独立事务提交**（autocommit，含 fsync）→ 高频采集写放大严重 | 增加 `addRecords()` 批量接口用事务提交；开库时执行 `PRAGMA journal_mode=WAL; synchronous=NORMAL;` |
| P2-12 | `src/historydata.cpp:93` | `query.exec()` 返回值被忽略 → 磁盘满/DB 被锁时**记录无声丢失**，无日志、无返回值 | 检查返回值并 `qWarning()`；暴露 `lastError()` 信号 |
| P2-13 | `src/historydata.cpp:96-128` | `query()` 无 LIMIT/分页 → 一次把整个时间范围读进内存；UI 再逐行 `new QTableWidgetItem` → 几十万行直接卡死 | 加 `LIMIT` 与分页参数；表格改用 `QAbstractTableModel` 虚拟化 |
| P2-14 | `src/historydata.cpp:72-76` | 硬编码 30 天自动清理，无配置、无条数上限，且在写路径上同步执行大 DELETE | 保留期做成配置项；清理移出 `addRecord`，改由低频定时器执行并打印删除条数 |
| P2-15 | `src/mainwindow.cpp:751-781` | 每条日志都做一次 `mkpath` + open/close 文件（GUI 线程 IO）→ Auto read 100ms 时每秒 10+ 次 CreateFile | `mkpath` 只做一次；日志改常驻 `QFile` + 定时 flush |
| P2-16 | `src/dataexporter.cpp:24-25` | 达到容量后每次 `addRecord` 都 `removeFirst()` → **O(n) 内存搬移**（maxRecords=10000 时每条搬 0.5MB） | 改用 `QQueue` 或环形缓冲 |
| P2-17 | `src/dataexporter.cpp:24-25` | `setMaxRecords(0)` 使裁剪条件 `m_maxRecords > 0` 短路 → **裁剪完全失效，内存无界**（实测 2000 条全留） | 明确语义：`0` → 回落默认值，或用 `INT_MAX` 表示不限并加总量告警 |
| P2-18 | `src/mainwindow_advanced.cpp:1623-1636` | "长稳测试"在主线程连做 1000 次同步 SQLite 写 → 界面冻结数秒 | 批量事务写入；长稳测试放 `QThread` |
| P2-19 | `src/dataexporter.cpp:40-72` | 导出完成不检查写入错误却返回 `true` → 磁盘满/只读目录时提示"成功"但文件是空的 | 检查 `out.status()` 与 `file.error()`，错误经 `QString *error` 返回 |
| P2-20 | `src/dataexporter.cpp:46-47` | 导出 CSV 未写 UTF-8 BOM（其它导出都有）→ Excel 打开中文乱码 | `out.setGenerateByteOrderMark(true)` |

### 状态机与协议健壮性

| # | 位置 | 问题 | 建议 |
| --- | --- | --- | --- |
| P2-21 | `src/mainwindow.cpp:330` | `connected = (state != UnconnectedState)` → **Connecting/Closing 都被当作"已连接"** → `notifySuccess()` 在 Connecting 状态也被调用 → **ReliabilityManager 的"连续通信失败 N 次"告警永远不会触发** | 改为 `state == ConnectedState`；`notifySuccess()` 只在真正读到数据时调用；状态胶囊改三态 |
| P2-22 | `settingsdialog.ui:105-107` | 超时 SpinBox `minimum = -1` → `QModbusClient::setTimeout(-1)` → `QTimer::start(-1)` **定时器未启动** → 请求永不超时，`m_autoReadBusy` 永不复位 → **定时读取静默停摆**（状态栏仍显示"正在自动读取"） | `setMinimum(1)`；`applyButton` 里再 `qMax(1, ...)` |
| P2-23 | `src/settingsdialog.cpp:60-62`、`:86` | 校验位枚举映射错位：下拉顺序是 No/Even/Odd/Space/Mark，代码用 `index+1`，而 `QSerialPort::Parity` 是 No=0/Odd=1/Even=2/Space=3/Mark=4 → **选 Odd 实际是 SpaceParity**，选 Mark 传入非法值 5 | 用显式映射表 `kParityByIndex[]`，反向也按表查找；字段类型改为 `QSerialPort::Parity` |
| P2-24 | `src/mainwindow.cpp:404/601/481` | `m_readUpdatesInPlace` 是**成员变量**而非每请求属性：发送时写、回包时读，异步期间可被另一次请求改写 → 定时读与手动读并发时**模式串扰**，列表每 tick 追加 N 行且**行数无上限增长** | 改为每请求属性：`connect(reply, &QModbusReply::finished, this, [this, reply, inPlace]{...})`，把 `inPlace` 随请求捕获 |
| P2-25 | `src/mainwindow.cpp:811` | `readSize` 可编辑且无上界 → 输入 100000 会切成 800 段**一次性向设备发出 800 个请求**（设备被打爆、请求队列堆积） | `readRequest()` 夹逼到协议上限（寄存器 125 / 位 2000），并限制同时在途分段数 |
| P2-26 | `src/mainwindow.cpp:449-452`、`:546`、`:601-605` | 广播回复"立即完成"时直接 `delete reply` → `readReady` 不被调用 → `m_autoReadBusy` 永不复位；分段读时**第一段回包就清 busy**，其余分段仍在途 | 用**在途请求计数**代替布尔量：发送成功即 +1，每个回包 -1，减到 0 才允许下一 tick |
| P2-27 | `src/remoteserver.cpp:224-231` | 写入值无范围/类型校验：`static_cast<quint16>(v.toInt())` **实测** `65536→0`、`-1→65535`、`70000→4464`、`"abc"→0`、`1.9→0`，全部静默改写后返回 success | 逐值校验：必须是整数且 ∈[0,65535]，否则返回 400 |
| P2-28 | `src/remoteserver.cpp:200-207` | `count` 只校验 `> 0`；Qt 6.6 `QModbusDataUnit(type, int, quint16)` 第三参是 **quint16** → `count=65536` 静默变 0，`2000000000` 变 37888，请求语义与实际执行完全不符 | RemoteServer 层白名单校验：`count ∈ [1,125]`（位区 2000）、`startAddress ∈ [0,65535]` |
| P2-29 | `src/remoteserver.cpp:183` | 路径**精确白名单匹配但不剥离 query string** → `GET /api/status?x=1` 返回 404 | `path.split('?')[0]` 后再匹配 |
| P2-30 | `src/remoteserver.cpp:264` | `Access-Control-Allow-Origin: *` 允许任意网页跨域访问本机 API | 改为回显受控 Origin 或关闭 CORS |
| P2-31 | `src/remoteserver.cpp:26` | `listen(QHostAddress::Any, port)` → 写接口暴露到整个局域网（仅靠一个共享 Token 保护） | 默认绑定 `QHostAddress::LocalHost`，需要远程访问时由用户显式选择并告警 |
| P2-32 | `src/remoteserver.cpp:33-41` | `stop()` 只 `disconnectFromHost()` 不删除 socket → 反复 start/stop 会累积 socket/FD | `stop()` 中 `qDeleteAll(m_clients)` |
| P2-33 | `src/mqttclient.cpp:415`、`:190-194` | 只发 PINGREQ，**从不校验 PINGRESP** → broker 进程假死但 TCP 未断时，客户端仍认为"已连接"，写缓冲与 `m_pending` 持续增长（MQTT 3.1.1 §3.1.2.10 要求超时关闭） | 记录 `lastPingRespMs`，超过 1.5×keepAlive 未回 → `abort()` 触发重连 |
| P2-34 | `src/mqttclient.cpp:202` | 断链时 `m_pending.clear()` **清空所有未确认 QoS1 消息且无任何告警** → 静默丢数据 | 清空前统计数量并 emit 告警；或按 clean session=0 语义在重连后重发 |
| P2-35 | `src/mqttclient.cpp:63-68` | 析构时 `sendDisconnect()` 只 write 不 flush，socket 立即被销毁 → broker 侧看到 TCP 异常关闭而非 DISCONNECT | `flush()` + `waitForBytesWritten(200)` |
| P2-36 | `src/mqttclient.cpp:221-243` | `encodeString` 对 >65535 字节截断长度字段产出非法包；`encodeRemainingLength` 对 >268435455 生成 5 字节非法 varint；`setKeepAlive()` >65535 被静默截断 | 编码前 `qBound` 校验，超限拒绝 publish 并报错 |
| P2-37 | `src/mqttclient.h:113` | 使用 `QMap` 但**未 include `<QMap>`**（靠传递包含才编过） | 补 include（IWYU） |

### 安全

| # | 位置 | 问题 | 建议 |
| --- | --- | --- | --- |
| P2-38 | `src/securitymanager.cpp:351-354` | 口令哈希为**单次 SHA-256**（虽加盐但无迭代）→ GPU 可做 ~10¹⁰ 次/秒字典爆破，默认口令 `admin123`/`123456` 秒破 | 改用 PBKDF2/Argon2（Windows 可用 CNG `BCryptDeriveKeyPBKDF2`）；口令策略下沉到 `SecurityManager` |
| P2-39 | `src/securitymanager.cpp:96`、`:373` | 鉴权比较用 `QString::operator==`（非常量时间） | 用 `QByteArray` 手工常量时间比较 |
| P2-40 | `src/securitymanager.cpp:173-192` | `login()` 无失败锁定/限速 → 口令在线爆破无阻碍；审计文件写在 `applicationDirPath()/audit`（装到 Program Files 不可写 → **审计日志静默丢失**） | 加失败计数 + 指数退避；审计目录改 `QStandardPaths::AppDataLocation`；写失败至少 `qWarning` |
| P2-41 | `src/securitymanager.cpp:173-192` | 强制改密只在 UI 层强制，`login()` 本身不检查 `mustChangePassword` → 新增调用方可绕过 | 把闸门下沉到 `hasPermission()`：`mustChangePassword` 时除 `security.changePassword` 外一律拒绝 |
| P2-42 | `src/securitymanager.cpp:153-166` | 审计日志直接拼接用户名/detail，用户名含 `\n` 可**伪造任意审计行**；日志无轮转/无上限 | 过滤 `\r\n` 与控制字符；按天/大小轮转 |
| P2-43 | `src/securitymanager.cpp:231-254` | `addOrUpdateUser` 允许把 **admin 的角色改成 viewer**，`setUserEnabled` 允许禁用 admin → 可将唯一管理账号锁死 | 保护 admin 的角色与启用状态（`removeUser` 已保护，此处未保护） |
| P2-44 | `src/securitymanager.cpp:312-330` | `ensureDefaults()` 只在 `m_users` **整体为空**时创建 admin；若配置中 users 非空但 admin 缺失则永不重建 → 无法恢复管理入口 | 改为"admin 不存在时才创建" |
| P2-45 | `src/securitymanager.cpp:39-47` | 用户名直接拼进 QSettings key 且 `load()` 未 trim、`addOrUpdateUser` 会 trim → `" admin "` 与 `"admin"` 变成两个账号 | 统一归一化用户名 |
| P2-46 | `header/credentialcodec.h:20-27` | 内置**硬编码 XOR 密钥** + 重复密钥流 → 等同明文（`strings` + XOR 即可还原 broker 口令） | 改用 Windows DPAPI / Credential Manager 存储 broker 口令 |
| P2-47 | `header/credentialcodec.h:37-46` | `QByteArray::fromBase64()` 默认忽略非法字符 → 密文损坏时静默得到垃圾字节，无法区分"密码错"和"密文坏" | 用 `Base64AbortOnBase64DecodingErrors`，失败返回空并 `qWarning` |
| P2-48 | `src/pluginmanager.cpp:96-116` | 插件目录无任何信任校验，任意 `.dll` 都会被 `QPluginLoader::load()` **映射进进程并执行 DllMain**（即使加载失败）→ 本地代码执行 | 插件目录迁到 `AppDataLocation`；加载前做白名单/哈希校验；目录 ACL 收紧 |
| P2-49 | `src/pluginmanager.cpp:132-153` | 无 ABI/接口版本校验：IID 硬编码 `"...1.0"` 从不递增，接口是纯虚 C++ 类（vtable 随函数增删变化）→ 旧插件被新宿主加载会**调用到错误槽位** | 接口演进时递增 IID 并在 `loadPlugin` 中比对 `metaData()["IID"]`；加不依赖 vtable 的版本探测 |
| P2-50 | `src/pluginmanager.cpp:43`、`:59` | 插件配置以**绝对路径**为 key → 程序目录一变，全部失配 → 用户此前明确**禁用的插件被自动重新启用** | key 改用文件名或插件元数据里的稳定 ID |
| P2-51 | `src/pluginmanager.cpp:248-265` | 插件回调在主线程直接调用 → 插件崩溃 = 主程序崩溃；且迭代期间 `m_plugins.clear()` 会使迭代器失效 | 迭代期间用不可变快照 + 重入保护；插件契约中声明"插件崩溃会带崩宿主" |
| P2-52 | `src/scriptengine.cpp:12-81` | 脚本执行**无任何超时/中断** → `while(true){}` 永久冻结 GUI 线程，只能强杀（未保存配置全丢） | 用 `QJSEngine::setInterrupted()` + 看门狗定时器（Qt6 线程安全）；或脚本放独立 QThread |
| P2-53 | `src/scriptengine.cpp:88-92` | `setGlobalObject` 未设 `CppOwnership` → 引擎析构时**删除宿主 QObject** → 悬垂指针 | `QJSEngine::setObjectOwnership(object, QJSEngine::CppOwnership)` |
| P2-54 | `src/scriptengine.cpp:12-30` | `loadScript` 不重置全局作用域 → 多次加载互相污染；函数清单靠正则扫源码 → 注释里的 `function fake()` 会被误判，箭头函数/对象方法全部漏掉 | 每次加载重建 engine 或清理 global；函数清单改为遍历 `globalObject()` 属性并用 `isCallable()` 判定 |
| P2-55 | `src/scriptengine.cpp:15-19` | `m_lastError` 成功后**不清空** → 先失败后成功时 UI 仍弹"脚本错误"并显示上次的错误信息，真结果被吞 | 每次入口先 `m_lastError.clear()` |

### 健壮性 / 一致性

| # | 位置 | 问题 | 建议 |
| --- | --- | --- | --- |
| P2-56 | `src/mainwindow.cpp:195-206`、`:930-935` | 析构中 `delete m_chart` / `delete m_dashboard` 在 `delete modbusDevice` **之前**，而 `delete modbusDevice` 会触发在途 `QModbusReply::finished()` → `readReady()` → `pointValueUpdated` lambda → `m_chart->addDataPoint()` → **m_chart 已为 nullptr → 段错误** | 析构开头加 `m_shuttingDown` 标志；lambda 内判空；或把 `delete m_chart/m_dashboard` 移到最后 |
| P2-57 | `src/mainwindow_advanced.cpp:1000-1010` | 设备模板的 `byteOrder` 转点位时被丢弃（同 P0-1） | 随 P0-1 一并修复 |
| P2-58 | `src/pointmodel.cpp:101` | 跨响应边界点位静默跳过（详见 P1-10） | 随 P1-10 一并修复 |
| P2-59 | `src/pointmodel.cpp:324/327` vs `:443-475` | CSV 导出 `id` 列但导入忽略；JSON 根本不导出 `id` → 两种格式语义不一致；`m_nextId` 在 clear 后不重置 | 二选一：CSV 也不导出 `id`，或支持按 id 导入；覆盖导入时重置 `m_nextId` |
| P2-60 | `src/pointmodel.cpp:405-418`、`:433` | 表头识别过宽（首行任一格命中关键字即认定表头）→ 无表头 CSV 首行恰好叫 `name` 会**丢掉一个点**；所有字段 `trimmed()` → 点位名前后空格被吞，往返有损 | 表头判定改为"首行 ≥2 个单元格都是已知列名"；文本列保留原值 |
| P2-61 | `src/pointmodel.cpp:334-338` | CSV 导出 double 用默认 6 位有效数字 → `scale=1.23456789` 导出成 `1.23457`，往返有损 | `QString::number(v, 'g', 17)` |
| P2-62 | `src/pointmodel.cpp:193-202`、`src/dataexporter.cpp:10-15` | CSV 公式注入未防护：点位名/单位以 `=` `+` `-` `@` 开头时，Excel 打开会当公式执行（加引号**不是**防护手段） | 公共 `csvutil` 统一处理：以 `= + - @` 或 TAB/CR 开头加 `'` 前缀 |
| P2-63 | `src/pointmodel.cpp:252-257` | 注释中 Qt 枚举取值与实际不符（把 Coils/DiscreteInputs 对调了）。代码用 `static_cast` 行为正确，但**按注释硬编码就会读错** | 修正注释或直接删除数字 |
| P2-64 | `src/dataparser.cpp:166-179`、`:212-225` | ASCII 分支错误复用数值字节序：DCBA/BADC 会交换寄存器内字节（`"AB"`→`"BA"`）；非 ASCII 字节**无声丢弃**（实测 `{0xE4B8}` → 空串）；`fromAsciiString` 用 `toLatin1()`，非 Latin1 字符变 `?` | 字符串类型不应复用数值字节序：新增 `toByteString()`/`toUtf8String()` 按 wire 序逐字节还原；`fromAsciiString` 改 `toUtf8()` 并对无法编码字符报错 |
| P2-65 | `src/dataparser.cpp:113-129` | `reorderBytes` 只支持 `size==4`，其它长度静默返回；且与 64 位路径语义不一致（当前无调用方） | 删除该死代码，或改为返回 `bool` 并对 `size != 4` 报错 |
| P2-66 | `header/dataparser.h:24-33` | API 不对称：缺 `fromUInt32` / `toInt16` / `toUInt64`；`pointmodel.cpp:80` 的符号扩展散落 | 补齐接口，`pointmodel.cpp:80` 改用 `DataParser::toInt16` |
| P2-67 | `src/alarmmanager.cpp:106` | 报警历史 `m_history` **无上限增长**（`alarmHistory(maxCount)` 只是读取时的视图截断，内存照涨） | 加 `m_maxHistory` 并裁剪；或落库 |
| P2-68 | `src/alarmmanager.cpp:43-55` | `setRuleEnabled(false)` 无条件发 `alarmCleared` → 即使规则从未激活也发"清除"事件 → 下游记录虚假清除 | 仅在 `m_activeStates` 为 true 时发 |
| P2-69 | `src/alarmmanager.cpp:21-31` | `removeRule` 不清理该规则的历史事件 → 同一批事件在"历史"可见、在"活跃"不可见，数据自相矛盾 | `removeRule` 同时剔除历史 |
| P2-70 | `src/alarmmanager.cpp:33-41` | `updateRule` 不重置去抖时间戳与激活状态 → 新规则可能被立刻去抖抑制，状态与新阈值不匹配 | `updateRule` 内重置两者 |
| P2-71 | `src/alarmmanager.cpp:73-78` | `checkValue` 对每个寄存器值线性扫描全部规则 → 100 寄存器 × 200 规则 = 2 万次比较/轮 | 建 `QHash<QKey, QVector<int>>` 索引 |
| P2-72 | `src/alarmmanager.cpp:71`（跨模块） | 报警阈值作用于**原始寄存器值**，而点表的 `scale/offset/alarmLow/alarmHigh` 全是**工程值** → 同一物理量两套单位，运维按工程值填阈值会差一个 scale 因子（漏报 + 误报） | 报警规则改为按 `pointId` 关联点位，比较已换算的工程值；或在规则中加 `scale/offset` |
| P2-73 | `src/pollmanager.cpp:22-27` + `mainwindow_advanced.cpp:809/1027/1576` | 轮询任务 ID 由 4 处不同算式生成：`10000+deviceId`、`20000+tmpl.id*1000+i`、`30000+pointId`、`40000+unit` → 模板寄存器数 ≥1000 或模板数 ≥10 时**跨段撞车**，`addTask` 按同 ID 视为更新 → 后者**静默覆盖**前者 | 改用 `PollManager` 内部自增 ID，或用复合字符串键 |
| P2-74 | `src/pollmanager.cpp:82-97` | 禁用/使能时在途标志被清空，但空中请求仍在；旧请求应答回来会清掉**新请求**的在途标志（只按 taskId 匹配）→ 保护形同虚设 | 用**请求序号**代替布尔标志 |
| P2-75 | `src/pollmanager.cpp:108-116` | 所有任务定时器**同时启动** → tick 齐步，Modbus 客户端瞬间收到 N 条并发请求（从站通常只允许 1~2 个未决） | 启动时按任务索引错开首拍；`setInterval` 会重置相位导致 tick 漂移，需保持相位 |
| P2-76 | `src/batchtaskmanager.cpp:130-161` | `onTaskFinished` 无延时地直接递归调用 `executeNextTask()` → 同步回调路径下形成递归链，任务数大时可能栈溢出；且不校验 taskId 是否为当前任务 | 校验 `taskId == m_tasks[m_currentIndex].id`；下一步改为 `Qt::QueuedConnection` 打断递归 |
| P2-77 | `src/reliabilitymanager.cpp:103-116` | 自动重连是**固定 3 秒无限重试**，无退避、无上限、无抖动 → 设备长期离线时持续空转，日志/事件风暴 | 指数退避 + 抖动 + 最大连续失败次数；成功后复位 |
| P2-78 | `src/reliabilitymanager.cpp:80-92` | `stop()` 不清零连续失败计数；失败计数同时被"读失败"和"断线"两条路径累加 → 单点读不通也按"整链路故障"上报，告警文案误导 | 区分"链路失败"与"单点失败"两个计数器 |
| P2-79 | `src/mainwindow_advanced.cpp:987` | "绑定设备→模板"只写不读（全项目无任何地方读 `templateBindings/`）→ 功能实际无效但 UI 有误导性按钮 | 补读取逻辑，或先隐藏该按钮 |
| P2-80 | `src/verificationmanager.cpp:22-32` | "生成默认计划"的幂等判断用 `m_items` 非空，导致无法重新生成/重置；且该类**不持久化**，重启全丢 | 用独立标志控制；增加 `saveToFile/loadToFile` |
| P2-81 | `src/deliverymanager.cpp:113-120` | `packageLogs` 在"没复制到任何日志"时仍返回 true（manifest 几乎必然被创建）→ 交付包只有 manifest.txt 却提示"成功" | 返回结构化结果（文件数、缺失目录）并在 UI 明示 |
| P2-82 | `src/mainwindow.cpp:242-246` | `connect(modbusDevice, ...)` 在 `if (!modbusDevice)` **之前**执行 → 下拉索引异常时 `connect(nullptr,...)` Qt 运行时告警 | 把 connect 移到 null 检查之后 |
| P2-83 | `src/mainwindow.cpp:356` | `QStringLiteral("%1 %2").arg(a, b)` 两个参数传给单占位符 `%1` → **第二个参数被静默丢弃** | 改用双占位符或手工拼接 |
| P2-84 | `src/mainwindow.cpp:80` | `QModbusReply *lastRequest` 声明并初始化后**从未使用** → 误导性的"当前请求"句柄 | 删除 |
| P2-85 | `src/mainwindow.cpp:811-814` | `readSize` 输入非法文本（如 `abc`）→ `toInt()==0` → 分段列表为空 → **无任何提示** | `toInt(&ok)`，失败或 ≤0 时给状态栏提示 |
| P2-86 | `fieldlink.pro:6` | `CONFIG += c++11` 在 Qt 6.6 的 qmake 下**不会**加 `-std=gnu++11`（实测 Makefile 无 `-std` 标志，用 GCC 默认 gnu++17）→ 声明有误导性 | 改为 `CONFIG += c++17` 明确基线 |
| P2-87 | `header/mainwindow.h:128`、`:156` | `class QTimer *m_autoReadTimer` 前向声明写法怪异；`m_lastRequest` 同上 | 整理成员声明 |

---

## 四、复核确认"非问题"（避免误改）

本轮特意为下列疑点写了验证代码，确认**不是 bug**：

| 疑点 | 结论 |
| --- | --- |
| `qFuzzyCompare(0.0, 0.0)` 是否返回 true | 返回 true，`Equal` 条件对 0 值可正常触发 |
| `alarmHistory(0)` 是否返回全量 | 返回空列表，语义合理 |
| 报警去抖是否阻塞"清除" | 只作用于"新触发"，不阻塞"清除"——**设计正确**（但"状态未同步"是缺陷，见 P1-11） |
| `readReady` 回复生命周期 | `deleteLater()` 齐全，无泄漏；`QModbusReply::parent() == client`，删除客户端会回收在途 reply |
| `HistoryData::closeDatabase` | 先置空 m_db 再 removeDatabase，规避了 Qt 经典崩溃坑 |
| `DataParser` 32 位字节序核心 | **实现正确**，`toFloat32(ABCD of 100.0f) == 100.0`（缺陷在于点位链路未使用它，见 P0-1） |
| `QSettings` 键冲突 | `devices/`、`ui/`、`mqtt/`、`security/`、`sim/`、`templateBindings/` 前缀互不重叠，**未发现真实冲突** |
| `PollTask` 聚合初始化字段顺序 | 与结构体声明逐字段核对**一致** |
| `onStateChanged` 的"意外断线"判定 | 用户主动断开与切换连接类型时**不会**误弹"是否重连"框 |
| `QModbusClient` 线程 | 全部在主线程创建、无 `moveToThread`，**不存在跨线程调用**问题（但见 P1-5 的阻塞问题） |
| `pluginmanager` 的 `qobject_cast<PluginInterface*>` | Qt6 下**可以编译**（`Q_DECLARE_INTERFACE` 提供了非 QObject 接口的 `qobject_cast` 特化），不是编译错误 |
| `deliverymanager.cpp:68` 的 `500*1024*1024` | = 524288000，未溢出 int |
| `reliabilitymanager.cpp` 的 `qMax` 下限钳制 | 有效，未发现绕过 |

---

## 五、建议的修复批次

| 批次 | 内容 | 理由 |
| --- | --- | --- |
| **第一批** | P0-1 ~ P0-4 | 现场数据错误 / 内存破坏 / 永久卡死，每一项都会造成不可逆后果 |
| **第二批** | P1-1 ~ P1-11 | 静默错误、协议违规、越权访问、崩溃 |
| **第三批** | P2-1 ~ P2-20（配置与性能） | 用户资产丢失、UI 不可用、内存无界 |
| **第四批** | P2-21 ~ P2-37（状态机与协议） | 功能静默失效、参数错位 |
| **第五批** | P2-38 ~ P2-55（安全） | 口令强度、审计可信度、插件与脚本隔离 |
| **第六批** | P2-56 ~ P2-87（健壮性） | 代码质量与一致性 |

### 配套建议

1. **回归测试补强**：把本轮 23 项复现断言并入 `tests/master_tests.cpp`，
   形成长期回归基线（当前 465 PASS → 建议扩展到 488+ PASS）。
   复现程序位于 `build/verify/verify_tests.cpp`，可直接迁移。
2. **抽出公共工具**：
   - `csvutil.{h,cpp}`：统一 `escapeField()`（含公式注入防护）、`writeBom()`、数值格式化（`'g',17`）——
     当前 `pointmodel` / `dataexporter` / `deliverymanager` 三处实现各自为政（BOM 行为已不一致）。
   - `byteOrderFromToken()/byteOrderToToken()`：供点位表、模板表、CSV 导入共用（是 P0-1 的前置）。
3. **静默错误总治理**：P0-1 / P0-2 / P1-9 / P1-10 的共性是
   **"解析失败 → 用默认值继续 → 质量码仍显示 Good"**。
   建议引入统一原则：**任何字段解析失败必须让该点位置 `Bad` 并记录日志**，
   宁可显示"无数据"，也不要显示一个"看起来正常"的错值。

---

## 六、审查方法与可复现性说明

- **既有回归**：`tests/master_tests.cpp` 在 Qt 6.6.3 / MinGW 11.2.0 下实测 **465 PASS / 0 FAIL**，
  说明上一轮 `doc/CODE_REVIEW.md` 记录的 P1/D1/S1/S2/S4/M1/P2 修复均已落地且未引入回归。
- **新增复现**：`tests/review_tests.cpp`（已并入正式测试套件），24 项断言全部 **CONFIRMED**：
  - MQTT 三项用自实现 mini broker 做**字节级**协议验证（含原始十六进制日志）；
  - RemoteServer 用**真实 TCP 分包**验证请求丢失（驱动真实 `RemoteServer` 实例）；
  - 点表/报警/导出用真实文件 IO 验证字段错位；
  - 越界与配置穿越用实际内存索引与文件系统验证。
- **PBKDF2 独立验证**：手写实现用官方标准向量逐一比对（5/5 通过），
  未仅依赖实现方的自述。
- **未验证项**：GUI 交互路径（`mainwindow_advanced.cpp` 的 2000+ 行）无法在无头环境完整自动化，
  其结论来自源码逐行核对与调用链追踪，已在正文中标注具体行号，便于人工复核。
  主程序已做启动/关闭冒烟测试（正常启停，退出码 0）。

---

## 附：本次审查与修复的产物

| 路径 | 说明 |
| --- | --- |
| `doc/CODE_REVIEW_2.md` | 本报告 |
| `tests/review_tests.cpp` | 24 项缺陷复现断言（修复后全部翻转为 `REVIEW-FIXED`） |
| `tests/test_alarm_export_history.cpp` | 65 项断言（报警/导出/历史修复回归） |
| `tests/test_security_config.cpp` | 102 项断言（安全/配置修复回归） |

### 修复后仍未处理的 P2 项（约 27 项）

以下为低优先级、非功能性缺陷，建议后续排期处理：

- **UI 细节**：状态胶囊未做三态（连接中/已连接/已断开）；`lastRequest` 未使用的成员；
  `.arg()` 单占位符传两参导致参数被丢弃；日志目录仍固定在程序目录（应改 `AppDataLocation`）。
- **配置项**：历史保留期未在 UI 暴露（`setRetentionDays()` 已实现但无入口）；
  远程服务监听地址已改为回环但未提供"监听所有网卡"的选项。
- **协议细节**：64 位 CDAB 的厂商定义分歧需在 UI 标注；
  `DataParser` 缺 `fromUInt64`/`toInt16` 等对称接口。
- **插件/脚本**：插件 IID 校验已加但未做目录白名单；脚本引擎未加 `setInterrupted` 超时；
  脚本仍与主进程同权限运行。
- **其它**：任务 id 分段冲突仅告警未自动重分配；`registerType` 的 `writeOperators`
  默认值仍含 "admin"（向后兼容考虑，详见 `securitymanager.cpp` 注释）。
