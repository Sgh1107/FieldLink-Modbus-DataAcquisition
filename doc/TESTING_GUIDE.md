# FieldLink 测试指南（无真实设备）

> 分支：`mcp` ｜ MCP 使用说明见 [MCP_GUIDE.md](MCP_GUIDE.md)
>
> 本指南解决"手头没有 Modbus 设备怎么测"的问题：**MCP 大部分功能不需要任何设备**，
> 需要 Modbus 通信的部分用内置模拟器补齐。

---

## 一、测试分层总览

| 层级 | 需要设备？ | 测什么 |
| --- | --- | --- |
| L0 协议与安全 | ❌ | MCP 握手/工具列表/资源/提示词、Token 鉴权、限流、Origin 防护、会话 |
| L1 模拟设备 | ❌（用模拟器） | Modbus 读/写工具、轮询采集、历史入库、报警触发、曲线/仪表盘 |
| L2 端到端 | ❌（用模拟器 + AI 客户端） | Claude Desktop 自然语言操作全流程 |
| L3 真机 | ✅ | 现场实测（时序、串口电气特性等） |

---

## 二、L0：无设备的 MCP 测试

启动 FieldLink → Advanced → MCP Service (AI) → 输入 Token（假设 `123456`）→ 允许写入 → 开启人工确认。

以下**全部不需要连接任何 Modbus 设备**：

```powershell
$H = @{ "X-Api-Token" = "123456"; "Content-Type" = "application/json" }
$U = "http://127.0.0.1:8180/mcp"

# 1. 握手（观察响应头里的 Mcp-Session-Id）
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"0"}}}'

# 2. 工具清单（应有 11 个）
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'

# 3. 系统状态（无需设备，返回未连接状态）
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_system_status","arguments":{}}}'

# 4. 设备列表 / 报警规则 / 历史统计（空库返回 0 条）
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"list_devices","arguments":{}}}'

# 5. 资源与提示词
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":5,"method":"resources/list"}'
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":6,"method":"prompts/list"}'

# 6. 安全验证
# 6a. 无 Token → 401（Token 模式下）
Invoke-RestMethod -Method Post -Uri $U -Body '{"jsonrpc":"2.0","id":7,"method":"tools/list"}'
# 6b. 写闸门关闭时调危险工具 → isError 结果
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"write_registers","arguments":{"serverAddress":1,"registerType":4,"startAddress":0,"values":[1]}}}'
# 6c. 未知工具 → isError
Invoke-RestMethod -Method Post -Uri $U -Headers $H -Body '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"no_such_tool","arguments":{}}}'
```

SSE 通知通道观察（另开一个终端，保持运行）：

```powershell
curl.exe -N -H "Accept: text/event-stream" -H "X-Api-Token: 123456" http://127.0.0.1:8180/mcp
```

再做一次 6b 的写操作调用，就能在 SSE 流里看到 `notifications/message` 的确认结果推送，同时主界面弹出**危险操作确认框**——点「No」，AI 客户端会收到"用户拒绝了此次写操作"的 isError；点「Yes」会继续走执行（未连接设备则返回连接错误，同样验证了链路）。

---

## 三、L1：用模拟器测完整采集链路

### 1. 启动模拟器

```powershell
python deploy\modbus_tcp_simulator.py            # 监听 0.0.0.0:1502，从站地址 1
# 可先跑协议自检：python deploy\modbus_tcp_simulator.py --selftest
```

寄存器布局（专门为测试可视化/报警设计）：

| 寄存器 | 行为 | 适合测什么 |
| --- | --- | --- |
| reg0 | 正弦"温度" 15.0~35.0°C 缓慢变化 | 实时曲线、仪表盘 |
| reg1 | 0~5000 随机游走 | 报警阈值触发 |
| reg2 | 恒定 42 | 数据核对 |
| reg3~99 | 规律变化 | 批量轮询 |

### 2. FieldLink 连接模拟器

1. 主界面连接类型选 **TCP**，地址填 `127.0.0.1:1502`
2. 从站地址填 `1`，寄存器类型 HoldingRegisters，地址 0，数量 5
3. 点连接 → 状态胶囊变绿
4. 手动读一次 → 数据应包含正弦变化的 reg0 和恒定 42 的 reg2

### 3. 验证各功能

- **轮询采集**：Data → Scheduled Polling 新建任务（HoldingRegisters, 地址 0, 数量 3, 周期 1000ms）→ 启动，观察日志滚动
- **实时曲线**：View → Real-time Chart，reg0 应呈正弦波形
- **历史数据**：轮询几分钟后 Data → History Query 查询，应有记录
- **报警**：Data → Alarm Config 新建规则（reg1 > 4000 → Critical）→ 等随机游走越过阈值 → 报警触发
- **MCP 写工具**：`tools/call write_registers` 写 reg10 = 1234 → 用 FC03 回读（模拟器响应正确写入）

### 4. RTU 测试（可选）

RTU 需要虚拟串口：安装 [com0com](https://sourceforge.net/projects/com0com/) 创建虚拟串口对（如 COM10↔COM11），FieldLink 用 COM10，模拟侧用支持 RTU 的工具（如 `pymodbus` 的串口服务端或商业软件 Modbus Slave）监听 COM11。**建议先用 TCP 完成所有测试**，RTU 仅在验证串口参数时需要。

---

## 四、L2：Claude Desktop 端到端

1. 模拟器 + FieldLink MCP 服务均已启动
2. 配置 `claude_desktop_config.json`（见 MCP_GUIDE.md 第四节），重启 Claude Desktop
3. 对话验证：
   - 「FieldLink 现在什么状态？」→ 应调用 `get_system_status`
   - 「读一下 1 号设备保持寄存器 0~4」→ `read_registers`，返回正弦温度和 42
   - 「把保持寄存器 10 写成 1234」→ 弹出**确认框**，拒绝时 Claude 会告诉你被操作员拒绝
   - 「最近一小时数据有什么趋势？」→ `query_history`（先轮询几分钟攒数据）
   - 「给 reg1 加一条大于 4000 的 Critical 报警」→ `add_alarm_rule` + 确认框
4. 触发 Prompts：在 Claude Desktop 输入 `/mcp__fieldlink__daily_report`（或对话要求生成日报）

---

## 五、常见问题

| 现象 | 处理 |
| --- | --- |
| 模拟器端口被占用 | 换端口：`--port 1503`，FieldLink 地址同步修改 |
| 连接成功但读失败 | 确认从站地址一致（模拟器默认 1） |
| 读到的 reg0 一直是同一个值 | 正常，周期 30 秒的慢正弦，等一会儿再看曲线 |
| MCP 返回 "Modbus device not connected" | 先在主界面点连接，MCP 的读/写依赖主界面连接 |
| 想测多从站 | 多开几个模拟器（`--port 1503 --unit 2`），FieldLink 设备管理器加多台设备 |
