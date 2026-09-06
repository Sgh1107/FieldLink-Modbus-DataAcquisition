# FieldLink master 分支 Code Review 报告

> 审查范围：master 分支全部核心模块（AlarmManager / SecurityManager / ReliabilityManager /
> MqttClient / DataExporter / PollManager / DeviceManager / HistoryData / BatchTaskManager /
> DataParser / 主窗口连接与读写链路）
> 配套交付：`tests/tests.pro` + `tests/master_tests.cpp` 测试套件（无 GUI 可运行）
> 状态：**测试全部通过（96 PASS / 0 FAIL），以下问题均经测试或代码走查确认，尚未修复，等待评审决策**

---

## 一、测试执行方式与结果

```powershell
# 构建
cd build\tests
qmake ..\..\tests\tests.pro && mingw32-make
# 运行
debug\fieldlink_tests.exe        # 退出码 0 = 全部通过
```

**结果汇总：96 PASS / 0 FAIL / 7 项已知问题被测试确认。**

覆盖矩阵：

| 模块 | 用例数 | 结论 |
| --- | --- | --- |
| AlarmManager（8 种条件/去抖/确认/历史） | 18 | ✅ 全过；去抖语义经复核为正确设计 |
| SecurityManager（Token/角色/权限/持久化） | 16 | ✅ 全过；默认凭据问题被确认（见 S1/S2/S4） |
| ReliabilityManager（意图门控 4 场景） | 4 | ✅ 全过（911d4fd 修复的回归测试） |
| MqttClient（内置 FakeBroker 线缆测试） | 10 | ✅ 全过：CONNECT/CONNACK、PUBLISH、retain、PINGREQ、认证拒绝 |
| DataExporter（CSV 导出/容量裁剪） | 9 | ✅ 全过 |
| PollManager（多任务调度/启停/禁用） | 7 | ✅ 全过 |
| DeviceManager（CRUD/持久化回环） | 8 | ✅ 全过 |
| HistoryData（SQLite 全链路） | 12 | ✅ 全过（含 DESC 排序语义） |
| BatchTaskManager（顺序/跳过禁用/JSON 持久化） | 9 | ✅ 全过 |
| DataParser（字节序） | 10 | ⚠️ 自往返通过，但发现 **P1 高危语义缺陷**（见下） |

复核后排除的疑点（写测试证明"不是 bug"，避免误改）：
- `qFuzzyCompare(0.0, 0.0)` 实际返回 true——`Equal` 条件对 0 值可正常触发
- `alarmHistory(0)` 实际返回空列表（而非全量），语义合理
- 报警去抖只作用于"新触发"，不阻塞"清除"——设计正确

---

## 二、确认的问题清单（按严重度排序）

### 🔴 P1（高）：DataParser 字节序实现与 Modbus 寄存器字节序语义不符

**现象（三条测试确认）**：
- `fromFloat32(100.0f, ABCD)` 未生成标准 ABCD 寄存器序（期望 `{0x42C8, 0x0000}`）
- `toFloat32(0x42C8, 0x0000, ABCD)` 未还原 100.0f（返回 ≈2.7e-41 的错误值）
- `toUInt32(0x1234, 0x5678, ABCD)` 得到 `0x78563412` 而非 `0x12345678`

**根因**：实现先 `memcpy` 取**主机内存字节序**（x86 为小端）的字节，再做字节序重排——
把"主机内存布局"和"Modbus 寄存器大端布局"两个概念混在一起。`fromX → toX` 自往返碰巧
自洽（DCBA 双重反转抵消），但对**真实设备按 ABCD/DCBA 上报的寄存器值，解析结果是错的**。

**影响**：设备模板/点表中 float32/int32/uint32 + 字节序（ABCD/DCBA/BADC/CDAB）换算功能
对真实设备全部不可信；UI 上显示的工程值会是乱值。测试里 `float32 自往返 PASS` 具有迷惑性。

**修复方向**：先按大端拼 `raw = (regHi << 16) | regLo`，再按字节序对 32 位整体重排后
reinterpret 为 float（DCBA = 32 位字节全反序、BADC/CDAB = 16 位半字交换）；`fromX` 为逆过程。

### 🟠 S1（中高）：远程 API 默认 Token 是源码常量

`SecurityManager::load()` 中 `security/apiTokenHash` 缺省值为 `hashToken("modbus-admin")`。
首次部署若操作员未修改 Token，任何读过源码的人都能通过 `/api/read` 未授权读取现场数据
（写默认关闭）。**建议**：默认哈希置空 + Token 未配置时 API 直接 403；或首次启动随机生成
并提示用户。测试证据：`S1: 默认 API Token 为源码常量 modbus-admin → CONFIRMED`。

### 🟠 D1（中）：`toFloat64` 忽略 ByteOrder 参数

`Q_UNUSED(order)`——8 字节 double 恒按大端拼装，与 float32 接口语义不一致（本就受 P1 影响）。
修复 P1 时应一并处理。

### 🟡 S2（中）：默认账号 admin/admin123

`ensureDefaults()` 创建默认管理员，密码为公开常量。建议：首次登录强制修改密码。

### 🟡 M1（中）：MQTT broker 断开期间日志刷屏

轮询任务运行中若 broker 断开，每条轮询数据上送都会触发 `MqttClient::publish` 的
`errorOccurred`（ERROR 级日志）。1000ms 轮询下日志窗口会被刷满。**建议**：未连接时
publish 静默丢弃（debug 级）或按次数聚合上报；恢复连接后记录一条"恢复"即可。

### 🟡 P2（中）：轮询不判空在途请求

`PollManager` 定时器到点即发 `pollRequest`，不检查上一请求是否完成。设备响应慢或超时
场景下请求会在 QModbusClient 内部排队堆积（表现为延迟越来越大）。**建议**：单一在途
请求保护（在途时跳过本 tick）或在任务级维护"上一请求完成"标志。

### 🟢 低优先级

| 编号 | 问题 | 建议 |
| --- | --- | --- |
| S4 | `addOrUpdateUser` 空密码创建的用户默认密码 123456（测试确认） | 强制必填密码或随机生成 |
| S3 | `hashToken/hashPassword` 无盐，SHA256 可被彩虹表预计算 | 加每用户随机盐 |
| A3 | `BitSet/BitClear` 位号来自 threshold1，`1 << bit` 无 0~31 校验，越界为 UB | 入参校验 |
| H1 | `HistoryData::openDatabase` 重复调用会触发 QSqlDatabase 重复连接名警告 | 入口处 `if (isOpen()) return true` |
| H2 | `addRecord` 内清理计数器是函数级 static（多实例共享） | 改为成员变量 |
| PM1 | `PollManager::addTask` 不校验 task.id 重复 | 注册时查重 |
| U1 | MQTT broker 密码明文存 QSettings（已文档化） | 可接入系统凭据库 |
| U2 | `executeRemoteWrite` 审计 operatorName 固定 "remote"，MCP/远程来源不可区分 | 传入来源标识 |
| U3 | DataExporter 的 timestamp 若含逗号会破坏 CSV 结构（当前格式安全） | 字段统一加引号 |

---

## 三、复核确认的"非问题"（避免误改）

1. **报警去抖**：语义为"同一规则两次触发的最小间隔"，只作用于新触发、不阻塞清除——正确
2. **Equal 对 0 值**：`qFuzzyCompare(0,0)` 返回 true，可正常触发（注意：与极小非零值比较仍
   受相对比较限制，属于 Qt 已知特性，寄存器整数值场景无影响）
3. **alarmHistory(0)**：返回空列表，语义合理
4. **readReady 回复生命周期**：`deleteLater()` 齐全，无泄漏
5. **HistoryData::closeDatabase**：先置空 m_db 再 removeDatabase，规避了 Qt 经典崩溃坑

---

## 四、未提交的改动清单

| 文件 | 说明 |
| --- | --- |
| `tests/tests.pro` | 测试工程（无 GUI，链接 Core/Network/Sql/SerialBus） |
| `tests/master_tests.cpp` | 96 项断言 + 7 项已知问题复核 |
| `doc/CODE_REVIEW.md` | 本报告 |
| `build/run_tests_build.ps1` | 本地构建脚本（build/ 下，不入库） |

**未修复任何产品代码**——按你的要求，所有问题仅记录在案，等确认后决定修复批次。

---

## 五、建议的处理批次

1. **立即修**：P1（DataParser，点表核心功能）+ S1（默认 Token，安全）
2. **第二批**：D1、S2（强制改密）、M1（日志刷屏）、P2（在途请求保护）
3. **随缘**：低优先级表
4. **测试套件**：建议随本报告一并提交入库，作为后续回归基线
