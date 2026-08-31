# FieldLink — 工业级 Modbus 数据采集与监控平台

基于 Qt 开发的工业级 Modbus 通信与数据采集平台，专为工业物联网场景设计，覆盖 **设备通信 → 轮询采集 → 数据存储 → 可视化监控 → 报警联动 → 远程运维** 全链路，开箱即用、可直接落地。

当前版本：**v2.1.0**（见 `main.cpp` 中 `setApplicationVersion`）

---

## 🔧 技术栈

| 技术 | 说明 |
| --- | --- |
| C++11 | 项目核心语言 |
| Qt 5.15.2 | 推荐套件 Desktop Qt 5.15.2 MinGW 64-bit；代码已做版本条件编译，**兼容 Qt 6.2+**（`QModbusRtuSerialMaster/Client` 差异自动适配） |
| Qt 模块 | SerialBus（Modbus 主站）、SerialPort（串口）、Sql（SQLite 历史/报警存储）、Network（远程 API）、Widgets、Qml（脚本引擎） |
| MinGW 8.1 64-bit | 编译工具链 |
| SQLite | 历史数据与报警事件持久化（QtSql 驱动，免安装） |

---

## ✨ 核心亮点

- ✅ **双通道工业通信**：Modbus TCP（网络）+ Modbus RTU（串口）双模式主站，多设备统一管理，连接参数（端口/波特率/校验位/超时/重试次数）全部可配置并持久化
- ✅ **自动轮询采集**：多轮询任务独立定时器驱动，可按任务配置从站地址、寄存器类型、起始地址、数量与周期，互不干扰
- ✅ **批量读写任务引擎**：批量读/写任务按顺序执行，支持任务上下移动排序、任务间延时，满足一拖多设备的批量操作场景
- ✅ **历史数据落库**：SQLite 按记录存储采集数据，支持按时间区间/从站/寄存器组合查询、最近 N 条快查、过期数据自动清理
- ✅ **智能报警系统**：8 种触发条件（大于/小于/等于/不等于/范围内/范围外/置位/清零）× 3 级严重度，支持去抖时间、报警确认、报警历史查询
- ✅ **专业数据可视化**：实时曲线 + 仪表盘（Dashboard），支持点位与图表/仪表绑定，数据一目了然
- ✅ **远程监控 API**：内置 HTTP JSON API 服务（状态查询/远程读/远程写），API Token 鉴权，远程写可独立开关，便于上位机/运维系统集成
- ✅ **完备安全体系**：用户/角色/权限三级模型，密码与 API Token 均哈希存储，敏感操作权限校验
- ✅ **高可靠运行**：自动重连 + 心跳保活 + 连续失败告警（ReliabilityManager），全局崩溃捕获与日志记录（CrashLogger），适合无人值守长期运行
- ✅ **脚本与插件扩展**：内置 QJSEngine 脚本控制台（可加载脚本文件、注册全局对象），标准 Qt 插件接口（数据回调 + 连接状态回调 + 读写设置），二次开发友好
- ✅ **设备模板/点表管理**：寄存器点表支持数据类型（uint16/int16/uint32/int32/float32/ascii）、字节序（ABCD/DCBA/BADC/CDAB）、缩放/偏移/工程单位换算
- ✅ **主题与本地化**：深色/浅色工业风主题一键切换（Fusion + QSS），配置自动记忆；内置 i18n 框架（lrelease 构建期编译翻译并嵌入资源）
- ✅ **交付工具链内置**：交付清单、运行环境检查、日志打包、发布说明/用户手册/维护手册自动生成、Windows 打包脚本一键产出

---

## 📦 编译与运行

### 1. 环境准备

- Qt **5.15.2** 开发环境（或 Qt 6.2+），安装时务必勾选以下模块：
  - **Qt Serial Bus**（Modbus 支持）
  - **Qt Serial Port**（串口支持）
  - **Qt SQL**（SQLite 支持）
- 编译器：MinGW 64-bit（推荐 Qt 自带 MinGW 8.1）
- Windows / Linux 均可编译运行

### 2. 获取源码

```bash
git clone <你的仓库地址>
cd FieldLink-Modbus-DataAcquisition
```

### 3. 编译构建

**方式一：Qt Creator 可视化编译（推荐）**

1. 打开项目根目录的 `fieldlink.pro`
2. 选择匹配的编译套件（Desktop Qt 5.15.2 MinGW 64-bit）
3. 点击左下角「构建项目」▶，自动完成 moc/编译/链接

**方式二：命令行编译**

```powershell
# Windows（先将 Qt\5.15.2\mingw81_64\bin 与 Tools\mingw810_64\bin 加入 PATH）
cd FieldLink-Modbus-DataAcquisition
mkdir build && cd build
qmake ../fieldlink.pro
mingw32-make -j8        # Linux 下使用 make -j8
```

### 4. 运行程序

- 编译产物位于构建目录（如 `build/Desktop_Qt_5_15_2_MinGW_64_bit-Debug/debug/fieldlink.exe`）
- 首次运行后按需在「设置」中配置串口/网络参数、采集任务与报警规则，即可正常使用
- 历史数据库为 SQLite 文件，无需额外安装数据库服务

### 快速上手流程

```
添加设备（TCP/RTU）→ 配置轮询任务 → 启动采集
     → 实时曲线/仪表盘监控 → 历史查询与 CSV 导出
     → 配置报警规则 → （可选）开启远程 API / 脚本控制台 / 插件
```

---

## 🎯 适用场景

- 工业物联网数据采集与监控终端
- PLC / 变频器 / 仪表等 Modbus 设备的上位机
- 工厂自动化产线数据看板与报警中枢
- 智能设备数据网关、远程运维接入平台
- 教学与二次开发的 Qt 工业通信参考项目

---

## 📝 项目结构说明

```
FieldLink-Modbus-DataAcquisition/
├── main.cpp                  # 程序入口：主题初始化、崩溃捕获、Modbus 日志开关
├── mainwindow.*              # 主窗口：连接控制、手动读写、菜单与功能入口
├── mainwindow_advanced.cpp   # 主窗口高级功能实现（仪表盘/点位/安全/交付等集成）
├── mainwindow.ui             # 主界面 UI 文件
│
├── ── 通信层 ──
├── devicemanager.*           # 设备管理：Modbus TCP/RTU 客户端生命周期与配置持久化
├── devicetemplate.*          # 设备模板：寄存器点表定义（类型/字节序/缩放/单位）
├── pollmanager.*             # 轮询采集：多任务独立定时器
├── batchtaskmanager.*        # 批量读写任务：顺序执行、排序、延时
├── writeregistermodel.*      # 写寄存器数据模型
├── dataparser.*              # 数据解析
│
├── ── 数据层 ──
├── historydata.*             # 历史数据：SQLite 存储与查询
├── dataexporter.*            # 数据导出（CSV）
├── configprofile.*           # 配置档案：整站配置保存/恢复
├── alarmmanager.*            # 报警管理：规则触发/去抖/确认/历史
│
├── ── 可视化 ──
├── realtimechart.*           # 实时曲线
├── dashboard.*               # 仪表盘
├── pointmodel.*              # 点位管理：点位与图表/仪表绑定
│
├── ── 扩展与远程 ──
├── remoteserver.*            # 内置 HTTP JSON API 服务（状态/读/写）
├── scriptengine.*            # QJSEngine 脚本控制台
├── plugininterface.h         # 插件标准接口
├── pluginmanager.*           # 插件加载与管理
│
├── ── 可靠性与安全 ──
├── reliabilitymanager.*      # 自动重连/心跳/连续失败告警
├── securitymanager.*         # 用户/角色/权限、Token 鉴权
├── crashlogger.*             # 全局崩溃捕获与日志
├── logviewer.*               # 运行日志查看器
│
├── ── 工程化工具 ──
├── verificationmanager.*     # 验证计划与报告导出
├── deliverymanager.*         # 交付清单/环境检查/手册生成/打包
├── deploy/package_windows.ps1  # Windows 发布打包脚本
├── thememanager.h            # 深色/浅色主题（Fusion + QSS）
├── settingsdialog.*          # 设置对话框
│
├── fieldlink.pro             # qmake 工程文件
├── fieldlink.qrc             # 资源文件（图标等）
├── style/                    # dark.qss / light.qss 主题样式
├── translations/             # 界面翻译（zh_CN，构建期嵌入资源）
├── images/                   # 界面图标资源
├── doc/                      # Modbus 文档
└── build/                    # 构建输出目录
```

---

## 📄 开源协议

本项目采用 **MIT License** 开源协议，可自由学习、二次修改与非/商用，二次分发请保留原作者版权信息。

---

## 📬 关于作者

欢迎 Star、Fork、Issue 反馈！你的支持是持续更新优化的最大动力 💪
