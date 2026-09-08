# FieldLink 使用手册（USAGE）

> 适用版本：v2.1.0+ ｜ 配套文档：`README.md`（功能总览）、`doc/MQTT_GUIDE.md`（MQTT 细节）、`doc/CODE_REVIEW.md`（质量报告）
> 本文面向操作员/工程师，覆盖**每个功能的操作步骤**以及**功能之间的搭配用法**（含内置模拟脚本联调方案）。

---

## 目录

1. 五分钟快速上手（全链路无硬件）
2. 设备连接（TCP/RTU）与断线处理
3. 手动读/写与「Auto read」定时读取
4. 轮询任务（PollManager）
5. 批量读写任务
6. 点位管理 + 实时曲线/仪表盘
7. 历史数据查询与 CSV 导出
8. 报警系统
9. MQTT 数据上送
10. 远程 HTTP API
11. 脚本控制台 / 插件
12. 用户、角色与安全
13. 模拟脚本参考
14. 功能搭配典型方案
15. 常见问题 FAQ

---

## 一、五分钟快速上手（全链路无硬件）

不用任何真实设备，用项目内置的模拟从站即可跑通 **连接 → 采集 → 曲线 → 报警 → 历史落库 → MQTT** 全链路：

```
第 1 步  启动 FieldLink → 菜单 Advanced → Simulator (Modbus Slave)
         → 点「启动模拟从站」（默认 127.0.0.1:1502，从站地址 1）

第 2 步  主界面连接类型选 TCP → 地址填 127.0.0.1:1502
         → 点「Connect」→ 状态芯片变绿「已连接」

第 3 步  服务器地址填 1，寄存器选 Holding Registers，起始地址 0，数量 3
         → 点「Read」能看到 reg0~reg2 的值（reg0 是正弦温度×10）

第 4 步  勾选「Auto read」（间隔 500ms）→ 菜单 View → Real-time Chart
         → 曲线开始滚动（reg0 正弦波）

第 5 步  （可选）Advanced → MQTT Publishing → 保存并连接内置测试 broker
         → python slave\mqtt_test_broker.py 观察 JSON 上送
```

模拟器的寄存器布局（模拟一台"温度采集器"）：

| 寄存器 | 行为 | 适合用来验证 |
| --- | --- | --- |
| reg0 | 正弦温度 15.0~35.0°C（×10 存储） | 实时曲线、仪表盘 |
| reg1 | 随机游走 0~5000 | 报警阈值触发 |
| reg2 | 恒 42 | 写入/回读对比 |
| reg3+ | 规律变化 | 批量读 |

---

## 二、设备连接（TCP/RTU）与断线处理

### TCP 连接
1. 连接类型下拉选 **TCP**；
2. 地址栏填 `IP:端口`（如 `192.168.1.10:502`，模拟从站 `127.0.0.1:1502`）；
3. 点「Connect」，按钮变为「Disconnect」，状态芯片变绿。

### RTU（串口）连接
1. 连接类型选 **Serial**；
2. 地址栏填串口名（如 `COM3`）；
3. 菜单 **Options** 里配置波特率/校验位/数据位/停止位/超时/重试次数（与从站一致）；
4. 点「Connect」。

### 断线处理逻辑（重要）

| 断线场景 | 程序行为 |
| --- | --- |
| 你自己点「Disconnect」 | 直接断开，不弹窗、不重连 |
| 连接失败（地址不通等） | 手动连接弹窗报错；自动重连期间仅状态栏提示 |
| **对端主动断开 / 网络中断**（此前已连上） | **弹窗询问"对方已主动断开连接，是否重连？"** —— 选「是」立即重连并恢复自动重连循环（之后若再次被断开仍会询问）；选「否」不再自动重连，直到你手动点「Connect」 |
| 自动重连中再点一次「Connect」 | 取消自动重连（给操作员的停止手段） |

自动重连周期默认 3 秒（`ReliabilityManager`），另有心跳读取（默认 5 秒）探测"假在线"；连续失败达到阈值会在仪表盘告警日志中记录。

> 验证方法：用模拟从站联调时，在 Simulator 面板点「停止模拟从站」= 对端主动断开 → FieldLink 立即弹窗。

---

## 三、手动读/写与「Auto read」定时读取

### 手动读取
1. 设置 **服务器地址**（从站地址）、**寄存器类型**（Coils / Discrete Inputs / Input Registers / Holding Registers）、**起始地址**、**数量**；
2. 点「Read」→ 结果显示在下方读取结果表（同时进入历史库与 MQTT）。

### 写寄存器
1. 切到写寄存器表格页，填入地址和值（支持单写 FC06 / 多写 FC16）；
2. 点「Write」→ **写成功后自动回读**，结果表立即刷新，读写闭环无需手动再读。

### 「Auto read」定时读取
1. 前提：设备已连接（未连接时勾选会被拒绝并提示）；
2. 勾选「Auto read」并在旁边数字框设置间隔（ms）；
3. 按当前主界面配置的地址/数量周期性读取，结果同样进曲线/历史库/MQTT；
4. 设备断开时会自动取消勾选，避免空转报错。

---

## 四、轮询任务（PollManager）

进入轮询任务管理，可创建多个**独立定时器**驱动的任务：

1. 新建任务：填任务名、从站地址、寄存器类型、起始地址、数量、周期（ms）、启用状态；
2. 任务可随时启停（全部启动/单任务启停）、修改、删除；
3. 每个任务读取成功后：数据写入历史库 → 上送 MQTT → 若命中报警规则则触发报警；
4. **在途保护**：上一条请求未完成时自动跳过本 tick，慢设备下不会堆积请求。

搭配用法：轮询任务 + 点位管理（第六节）= 点位自动刷新曲线/仪表盘。

---

## 五、批量读写任务

批量任务把多个读/写操作组成**顺序执行**的队列：

1. 添加任务：类型（读/写）、从站地址、寄存器、地址、数量/值、**任务间延时**（ms）、启用状态；
2. 支持任务上移/下移排序；
3. 点「执行」按顺序跑：禁用任务自动跳过；每个任务完成后有结果（成功/失败 + 读到的值）；
4. 任务列表以 JSON 持久化，重启后还在。

典型场景：一拖多设备巡检（读设备 A → 延时 100ms → 读设备 B → 写设备 C 设定值…）。

---

## 六、点位管理 + 实时曲线/仪表盘（可视化三件套）

点位管理是**可视化绑定的中枢**（Advanced → Point Manager）：

### 1. 添加点位
填名称、所属从站地址、寄存器类型、地址、数量，以及：
- **数据类型**：uint16 / int16 / uint32 / int32 / float32 / ascii
- **字节序**：ABCD / DCBA / BADC / CDAB（32 位类型生效）
- **缩放/偏移/工程单位**：如 raw=250, scale=0.1 → 显示 25.0°C
- **报警上下限**（alarmLow / alarmHigh）
- **存档开关与存档周期**（秒）

### 2. 点位详情页的一排绑定按钮

| 按钮 | 作用 |
| --- | --- |
| **趋势曲线** | 为该点位创建一条曲线序列并弹出曲线窗口（按点位 id 自动配色，最多 500 点） |
| **仪表盘** | 按点位量程创建一个仪表，绿/黄/红三色随报警阈值变化 |
| **轮询** | 一键生成该点位的轮询任务（周期取存档间隔） |
| **报警** | 一键生成"超出上下限"报警规则 |
| **存档** | 编辑点位存档参数 |

### 3. 推荐搭配（一键监控一个点位）
```
添加点位 → 点「轮询」（开始采集）→ 点「趋势曲线」（看趋势）
        → 点「仪表盘」（看实时值）→ 点「报警」（超限自动告警）
```
之后每次轮询读到新值：曲线追加数据点、仪表刷新、命中规则即触发报警事件。

> 曲线默认 Y 轴量程 0~100，寄存器原始值较大时看起来像"贴顶直线"属正常，可通过量程设置/缩放系数调整显示范围。

---

## 七、历史数据查询与 CSV 导出

- 所有读取成功的数据（手动读、Auto read、轮询、批量读）**自动写入 SQLite**（程序目录 `history.db`，免安装）；
- 菜单 View → 历史查询：按**时间区间 / 从站地址 / 寄存器类型 / 起始地址**组合查询，也可"最近 N 条"快查；
- 查询结果可**导出 CSV**（所有字段带引号转义，Excel 安全）；
- 超过 30 天的旧数据在写入时自动清理（每 1000 条触发一次检查）。

---

## 八、报警系统

### 规则要素
- **触发条件 8 种**：大于 / 小于 / 等于 / 不等于 / 范围内 / 范围外 / **位置位（BitSet）** / **位清零（BitClear）**（位号 0~31，越界规则不会触发也不会崩溃）；
- **严重度 3 级**：Info / Warning / Critical；
- **去抖时间**（ms）：同一规则两次触发的最小间隔，防止边界抖动刷屏。

### 操作
1. 在报警管理界面（或从点位一键生成）添加规则：绑定从站地址 + 寄存器类型 + 地址 + 条件 + 阈值；
2. 轮询/Auto read 的每个读取值都会过一遍规则；
3. 触发时：报警事件入历史、仪表盘告警日志记录、（若启用 MQTT）上送 `fieldlink/alarm/triggered`；
4. 支持单条确认 / 全部确认 / 报警历史查询；条件解除后自动"清除"。

搭配建议：reg1（随机游走）配一条"大于 4000 报警"规则，跑几分钟必触发，方便演练报警→MQTT 全链路。

---

## 九、MQTT 数据上送

### 配置与使用
1. 菜单 **Advanced → MQTT Publishing**；
2. 填 Broker 地址（默认 `127.0.0.1:1883`）、ClientID（留空自动生成）、用户名/密码（**保存后混淆存储，不明文落盘**）、主题前缀（默认 `fieldlink/`）、KeepAlive；
3. 点「保存并连接」→ 之后每次启动自动重连 broker；「断开」停止上送。

### 自动发布的主题

| 主题 | 内容 | retain |
| --- | --- | --- |
| `{前缀}data/{从站}/{寄存器类型}/{起始地址}` | 采集数据 JSON | 否 |
| `{前缀}alarm/triggered` | 报警事件 JSON | 否 |
| `{前缀}status` | 设备连接状态 | **是**（订阅方上线即得最新状态） |

数据消息示例：
```json
{"timestamp":"2026-09-08T10:00:00.123","serverAddress":1,
 "registerType":"HoldingRegisters","startAddress":0,"count":3,"values":[250,1737,42]}
```

### 测试方法（三选一）
```powershell
# 方法 1：内置迷你 broker（推荐，会打印每条报文）
python slave\mqtt_test_broker.py --port 1883

# 方法 2：mosquitto
mosquitto_sub -t "fieldlink/#" -v

# 方法 3：单元测试（进程内 FakeBroker，CI 自动跑）
build\release\fieldlink_tests.exe
```

可靠性设计：断线每 5 秒自动重连（认证被拒不重试）；断连期间消息**静默丢弃不刷屏**（每个断连周期只报一次告警）；支持 QoS 0 / QoS 1（QoS1 有 PUBACK 确认 + 超时 DUP 重发）；KeepAlive/2 周期心跳。

---

## 十、远程 HTTP API

内置 HTTP JSON API 服务（Advanced → Remote Service 开关）：

| 端点 | 方法 | 说明 |
| --- | --- | --- |
| `/api/status` | GET | 软件状态/连接状态/统计 |
| `/api/read` | POST | 远程读寄存器（body: serverAddress/registerType/startAddress/count） |
| `/api/write` | POST | 远程写寄存器（**独立开关**，默认关闭） |

- 鉴权：请求头带 `Authorization: Bearer <Token>`（或 body 里 `token` 字段）；
- **安全默认**：未配置 Token 时所有 API 直接 401/403；Token 在安全管理里设置（加盐哈希存储）；
- 远程写默认关闭，开启时会二次确认；所有远程操作写入审计日志（`audit/日期.audit.log`）。

curl 示例：
```bash
curl http://127.0.0.1:8080/api/status -H "Authorization: Bearer <你的Token>"
curl -X POST http://127.0.0.1:8080/api/read -H "Authorization: Bearer <你的Token>" \
     -H "Content-Type: application/json" \
     -d '{"serverAddress":1,"registerType":4,"startAddress":0,"count":3}'
```

---

## 十一、脚本控制台 / 插件

- **脚本控制台**（Advanced → Script Console）：内置 QJSEngine，可交互执行 JS、加载 `.js` 脚本文件，注册了全局对象供脚本调用软件能力（读写、查询等）；
- **插件**（Advanced → Plugin Manager）：把实现了 `PluginInterface`（`onDataReceived` / `onConnectionStateChanged` 等回调）的 Qt 插件 DLL 放进程序目录 `plugins/` 即可被加载，适合做协议转换、数据二次分发。

---

## 十二、用户、角色与安全

- 三级模型：**用户 / 角色 / 权限**。内置角色：admin（全部权限）、engineer、operator、viewer（只读）；
- 首次启动生成默认账号 **admin / admin123**，**登录成功会强制要求修改密码**（取消改密则本次登录失败）；
- 新建用户不填密码 → 初始密码 123456，同样强制改密；
- 密码与 API Token 均为**加盐 SHA256** 存储（每用户独立随机盐，旧无盐哈希登录时自动透明升级）；
- 敏感操作（远程写、脚本执行、安全管理…）都走 `requirePermission` 校验并留审计痕迹。

---

## 十三、模拟脚本参考

### 1. Modbus TCP 从站模拟器（`slave/modbus_tcp_simulator.py`，标准库零依赖）

```powershell
python slave\modbus_tcp_simulator.py                    # 监听 0.0.0.0:1502，从站 1
python slave\modbus_tcp_simulator.py --port 502         # 标准 502 端口
python slave\modbus_tcp_simulator.py --unit 2           # 第二台从站
python slave\modbus_tcp_simulator.py --selftest         # 内置 9 项协议自检（不需 FieldLink）
```

运行后控制台实时打印收到的请求；Ctrl+C 退出 = **对端主动断开**（可用来验证断线弹窗）。

### 2. MQTT 迷你 broker（`slave/mqtt_test_broker.py`）

```powershell
python slave\mqtt_test_broker.py                        # 0.0.0.0:1883
python slave\mqtt_test_broker.py --port 11883           # 指定端口
python slave\mqtt_test_broker.py --user test --pass secret   # 开启认证校验
```

打印 CONNECT/PUBLISH/PINGREQ 全部报文；Ctrl+C 退出可验证客户端断线重连与消息丢弃告警。

### 3. 多从站联调（模拟一拖多）
```powershell
start python slave\modbus_tcp_simulator.py --port 1502 --unit 1
start python slave\modbus_tcp_simulator.py --port 1503 --unit 2
```
在 FieldLink 里建两个轮询任务分别指向两台"设备"。

---

## 十四、功能搭配典型方案

**方案 A：车间温度看板（本地无人值守）**
```
模拟从站/真实设备 → 轮询任务(1s) → 点位绑定仪表盘投屏
                                └→ 报警规则(>35°C Critical) → 报警历史
```

**方案 B：数据上送 IoT 平台**
```
轮询任务 → MQTT(fieldlink/data/...) → 平台订阅
设备连/断 → MQTT(fieldlink/status, retain) → 平台实时感知现场在线状态
报警触发 → MQTT(fieldlink/alarm/triggered) → 平台告警推送
```

**方案 C：远程运维**
```
远程 API(/api/status /api/read) + Token 鉴权 → 运维脚本巡检
（按需开启 /api/write）→ 远程下发设定值 → 本地写后自动回读闭环
```

**方案 D：工艺参数批量下发**
```
批量任务：写 A 设备设定值 → 延时 → 读回校验 → 写 B 设备…（JSON 持久化，可复用）
```

**方案 E：教学/二开联调环境（零硬件）**
```
modbus_tcp_simulator.py + mqtt_test_broker.py 两个终端
+ FieldLink 全功能 → 曲线/报警/MQTT/远程 API 全链路演示
```

---

## 十五、常见问题 FAQ

**Q1：曲线是一条贴顶的直线？**
Y 轴量程默认 0~100，寄存器原始值 0~65535。用点位的缩放系数把值换算到物理量程，或调整曲线量程。

**Q2：MQTT 连上了但没有 data 消息？**
data 消息由**轮询/批量读取成功**触发，先确认有轮询任务在跑或勾选了 Auto read。

**Q3：被对端断开后不想每次弹窗？**
弹窗只在"此前已成功连接且非本人操作"时出现；选「否」后本次不再自动重连。无人值守场景建议保持自动重连策略。

**Q4：远程 API 一直 401/403？**
未配置 Token 或 Token 错误。到安全管理里设置 Token（设置后加盐哈希存储），远程写还需单独开启。

**Q5：登录被要求改密码？**
默认账号 admin/admin123 与初始密码 123456 都带强制改密标记，改完即解除。

**Q6：历史数据存哪？会不会无限膨胀？**
程序目录 `history.db`（SQLite）；超过 30 天的数据在写入过程中自动清理。

**Q7：测试怎么跑？**
`tests/` 目录：`qmake tests.pro && mingw32-make`，运行 `fieldlink_tests.exe`，退出码 0 = 全部通过；CI（GitHub Actions, Qt 6）每次 push 自动回归。


