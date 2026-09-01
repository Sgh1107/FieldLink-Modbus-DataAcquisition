# FieldLink MQTT 数据上送指南

> 分支：`master` ｜ 实现：`header/mqttclient.h`（零外部依赖的 MQTT 3.1.1 发布端）

---

## 一、功能概述

FieldLink 内置了一个**极简 MQTT 3.1.1 发布端客户端**（QoS 0），无需引入任何第三方库，即可把采集数据与报警事件实时上送到 MQTT broker，供组态软件、IoT 平台、Python 脚本等订阅消费。

**主题结构**（前缀可配置，默认 `fieldlink/`）：

| 主题 | 内容 | 触发时机 | retain |
| --- | --- | --- | --- |
| `{前缀}data/{从站}/{寄存器类型}/{起始地址}` | 采集数据 JSON（时间戳/地址/值数组） | 轮询任务、批量读取成功后 | 否 |
| `{前缀}alarm/triggered` | 报警事件 JSON（规则/严重度/触发值） | 报警触发 | 否 |
| `{前缀}status` | 设备连接状态 | 现场连接/断开 | **是**（订阅方上线即得最新状态） |

数据消息示例：

```json
{"timestamp":"2026-09-01T21:00:00.123","serverAddress":1,"registerType":"HoldingRegisters","startAddress":0,"count":3,"values":[248,1737,42]}
```

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

## 四、测试（无需真实 broker）

仓库附带标准库实现的迷你 broker `deploy/mqtt_test_broker.py`，会打印收到的每条消息：

```powershell
# 终端 1：启动测试 broker（可选开启认证）
python deploy\mqtt_test_broker.py --port 1883
python deploy\mqtt_test_broker.py --port 1883 --user test --pass secret

# 终端 2：FieldLink MQTT 设置里指向 127.0.0.1:1883 连接，启动轮询任务
# broker 终端将实时打印：
# CONNECT client=fieldlink-xxxx proto=MQTT level=4 clean=True
# PUBLISH topic=fieldlink/data/1/HoldingRegisters/0 retain=0 qos=0 bytes=96 payload={...}
# PINGREQ client=fieldlink-xxxx
```

有 mosquitto 的话用 `mosquitto_sub -t "fieldlink/#" -v` 观察效果相同。

## 五、已知边界（刻意精简）

- 仅实现**发布端 QoS 0**（遥测高频数据的标准选择），无订阅/无 PUBACK 重发
- MQTT 3.1.1（协议级别 4），兼容 mosquitto / EMQX / HiveMQ 等主流 broker
- 密码以明文存于本地 QSettings（与大多数桌面工具一致），如需加密存储可后续接入 SecurityManager

> 本实现已通过线缆级协议自测：CONNECT/CONNACK、QoS0 发布、retain 标志、PINGREQ 心跳、
> 认证拒绝处理，均使用 mini broker 逐字节验证。
