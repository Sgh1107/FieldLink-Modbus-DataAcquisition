# FieldLink MQTT 数据上送指南

> 分支：`master` ｜ 实现：`header/mqttclient.h`（零外部依赖的 MQTT 3.1.1 发布端）

---

## 一、功能概述

FieldLink 内置了一个**极简 MQTT 3.1.1 发布端客户端**（QoS 0/1），无需引入任何第三方库，即可把采集数据与报警事件实时上送到 MQTT broker，供组态软件、IoT 平台、Python 脚本等订阅消费。（完整使用说明见 `doc/USAGE.md`）

**主题结构**（前缀可配置，默认 `fieldlink/`）：

| 主题 | 内容 | 触发时机 | retain |
| --- | --- | --- | --- |
| `{前缀}data/{从站}/{寄存器类型}/{起始地址}` | 主连接采集数据 JSON（时间戳/设备/地址/值数组） | 轮询任务、批量读取成功后 | 否 |
| `{前缀}data/d{设备ID}/{从站}/{寄存器类型}/{起始地址}` | 多设备采集数据 JSON | 多设备轮询/批量读取成功后 | 否 |
| `{前缀}alarm/triggered` | 报警事件 JSON（规则/严重度/触发值） | 报警触发 | 否 |
| `{前缀}status` | 设备连接状态 | 现场连接/断开 | **是**（订阅方上线即得最新状态） |

数据消息示例：

```json
{"timestamp":"2026-09-01T21:00:00.123","deviceId":0,"serverAddress":1,"registerType":"HoldingRegisters","startAddress":0,"count":3,"values":[248,1737,42]}
```

> 主连接（`deviceId=0`）沿用原主题格式，保持向后兼容；多设备使用 `data/d{设备ID}/...` 主题，JSON 同时携带 `deviceId`，订阅方可据此区分同从站地址的不同设备。

## 二、配置与使用

1. 菜单 **Advanced → MQTT Publishing**
2. 填写 Broker 地址（默认 `127.0.0.1:1883`）、ClientID（留空自动生成）、可选的用户名/密码、主题前缀、KeepAlive
3. 点「保存并连接」——状态标签显示连接结果；此后程序每次启动会**自动重连** broker
4. 「断开」停止上送（`mqtt/enabled` 置为 false）

运行日志（Log Viewer）会记录连接/断开/错误事件；认证被拒等不可恢复错误不会无限重连。

## 三、可靠性设计

- **自动重连**：broker 断开后每 5 秒重试；CONNACK 拒绝（密码错误/未授权）时停止重试并报错
- **心跳保活**：每 KeepAlive/2 秒发送 PINGREQ（默认 60s → 30s 一次）
- **发送降级**：broker 未连接时 `publish` 返回 false 并记录日志，不阻塞采集主流程
- **clean session**：每次连接使用全新会话，避免离线期消息堆积冲击订阅方

## 四、测试（无需真实 broker，也无需 MQTTX）

**不需要安装 mosquitto / EMQX，也不需要 MQTTX 之类的客户端。** 仓库自带一个纯标准库实现的
迷你 broker `slave/mqtt_test_broker.py`（零第三方依赖，只要 Python 3），并提供了三层验证：

| 层 | 怎么跑 | 验什么 | 耗时 |
| --- | --- | --- | --- |
| 协议单元层 | 测试套件里的 FakeBroker（进程内） | CONNECT/PUBLISH/PINGREQ/PUBACK 编解码、retain、QoS1 闭环、未连接丢弃告警合并 | 秒级 |
| 端到端层 | `tools/mqtt_e2e.cpp` + 迷你 broker 子进程 | 真实 socket 路径、认证接受/拒绝、用户意图门控、8KB 大载荷、UTF-8 主题、二进制载荷 | ~20s |
| 手工联调层 | 手动起 broker + 主程序界面 | 真实采集数据是否按主题上送、QoS/retain 是否符合预期 | 看情况 |

一键脚本（会自动编译、拉起 broker、跑完并核对 broker 侧实际收到的条数）：

```powershell
# Windows（PowerShell，无需 Git Bash）
powershell -ExecutionPolicy Bypass -File tools\mqtt_test.ps1                 # 两层全跑
powershell -ExecutionPolicy Bypass -File tools\mqtt_test.ps1 -Mode e2e      # 只跑端到端
powershell -ExecutionPolicy Bypass -File tools\mqtt_test.ps1 -Mode manual -Port 18883  # 手工联调
```

```bash
# Linux / macOS / Git Bash / WSL
bash tools/mqtt_test.sh              # 两层全跑
bash tools/mqtt_test.sh --e2e        # 只跑端到端
bash tools/mqtt_test.sh --manual     # 手工联调
```

手工联调（只想亲眼看报文时）：

```powershell
# 终端 1：启动测试 broker（可选开启认证），会实时打印收到的每条 PUBLISH
python -u slave\mqtt_test_broker.py --port 1883
python -u slave\mqtt_test_broker.py --port 1883 --user test --pass secret

# 终端 2：FieldLink MQTT 设置里指向 127.0.0.1:1883 连接，启动轮询任务
# broker 终端将实时打印：
# CONNECT client=fieldlink-xxxx proto=MQTT level=4 clean=True
# PUBLISH topic=fieldlink/data/1/HoldingRegisters/0 retain=0 qos=0 bytes=96 payload={...}
# 多设备：PUBLISH topic=fieldlink/data/d2/1/HoldingRegisters/0 ... payload 含 "deviceId":2
# PINGREQ client=fieldlink-xxxx
```

有 mosquitto 的话用 `mosquitto_sub -t "fieldlink/#" -v` 观察效果相同；迷你 broker 也支持
SUBSCRIBE 与 `+`/`#` 通配符，会按订阅把匹配的 PUBLISH 转发回来，所以不装 MQTTX 也能用
一个小脚本当订阅端。

**什么情况才需要真实 broker**：验证与第三方 broker 的兼容性（mosquitto/EMQX/HiveMQ）、
TLS/WebSocket、集群与 ACL、以及订阅端的真实投递行为。迷你 broker 只实现发布端所能触发的
最小子集（CONNECT/CONNACK、PUBLISH、PUBACK、PINGREQ/PINGRESP、SUBSCRIBE 转发）。

## 五、已知边界（刻意精简）

- 发布端 QoS 0（即发即弃）/ **QoS 1**（报文标识符 + PUBACK 确认，2 秒超时 DUP 重发，重发 5 次仍无确认则丢弃并告警）；无订阅
- MQTT 3.1.1（协议级别 4），兼容 mosquitto / EMQX / HiveMQ 等主流 broker
- broker 密码经 `header/credentialcodec.h` 混淆后存于本地 QSettings（`enc:v1:` 前缀，历史明文读取时兼容），如需强加密可后续接入系统凭据库

> 本实现已通过线缆级协议自测：CONNECT/CONNACK、QoS0/QoS1 发布、PUBACK、retain 标志、
> PINGREQ 心跳、认证拒绝处理、断连静默丢弃，均使用进程内 mini broker 逐字节验证（155 项回归）。
