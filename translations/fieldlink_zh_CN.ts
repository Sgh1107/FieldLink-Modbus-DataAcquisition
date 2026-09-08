<?xml version='1.0' encoding='utf-8'?>
<TS version="2.1" language="zh_CN" sourcelanguage="en">
<context>
    <name>DeviceSimulatorPanel</name>
    <message>
        <source>Device Simulator (Modbus Slave)</source>
        <translation>模拟设备（Modbus 从站）</translation>
    </message>
    <message>
        <source>python or absolute path</source>
        <translation>python 或绝对路径</translation>
    </message>
    <message>
        <source>Path to modbus_tcp_simulator.py</source>
        <translation>modbus_tcp_simulator.py 路径</translation>
    </message>
    <message>
        <source>Python</source>
        <translation>Python</translation>
    </message>
    <message>
        <source>Simulator script</source>
        <translation>模拟器脚本</translation>
    </message>
    <message>
        <source>Port</source>
        <translation>端口</translation>
    </message>
    <message>
        <source>Slave address</source>
        <translation>从站地址</translation>
    </message>
    <message>
        <source>Start slave simulator</source>
        <translation>启动模拟从站</translation>
    </message>
    <message>
        <source>Stop</source>
        <translation>停止</translation>
    </message>
    <message>
        <source>Not running</source>
        <translation>未运行</translation>
    </message>
    <message>
        <source>Running (PID %1)</source>
        <translation>运行中（PID %1）</translation>
    </message>
    <message>
        <source>Tip: after starting, connect the main window via TCP to 127.0.0.1:%1 with slave address %2.</source>
        <translation>提示：启动后主界面连接类型选 TCP，地址填 127.0.0.1:%1，从站地址填 %2。</translation>
    </message>
    <message>
        <source>Select simulator script</source>
        <translation>选择模拟器脚本</translation>
    </message>
    <message>
        <source>Python (*.py)</source>
        <translation>Python (*.py)</translation>
    </message>
    <message>
        <source>Error: invalid simulator script path. Please select modbus_tcp_simulator.py.</source>
        <translation>错误：模拟器脚本路径无效，请选择正确的 modbus_tcp_simulator.py</translation>
    </message>
    <message>
        <source>Stopped</source>
        <translation>已停止</translation>
    </message>
    <message>
        <source>Simulator process exited (exit code %1).</source>
        <translation>模拟从站进程已退出（exit code %1）。</translation>
    </message>
    <message>
        <source>Hint: if the port is occupied, another service is already listening — try a different port.</source>
        <translation>提示：若端口被占用，说明已有服务监听该端口，请更换端口。</translation>
    </message>
    <message>
        <source>Error: cannot start the simulator. Please check the Python/script path.</source>
        <translation>错误：无法启动模拟器，请检查 Python/脚本路径。</translation>
    </message>
    <message>
        <source>Failed to start</source>
        <translation>启动失败</translation>
    </message>
    <message>
        <source>Browse...</source>
        <translation>浏览...</translation>
    </message>
    <message>
        <source>=== Slave simulator running ===</source>
        <translation>=== 模拟从站已启动 ===</translation>
    </message>
    <message>
        <source>Listen: 127.0.0.1:%1   Slave unit: %2</source>
        <translation>监听: 127.0.0.1:%1   从站地址: %2</translation>
    </message>
    <message>
        <source>Simulated data: reg0 = temperature sine (15.0~35.0°C x10), reg1 = random walk, reg2 = 42, reg3-99 = pattern</source>
        <translation>模拟数据：reg0=正弦温度(15.0~35.0°C×10) reg1=随机游走 reg2=42 reg3+=规律变化</translation>
    </message>
    <message>
        <source>Main window connect: TCP 127.0.0.1:%1, slave address %2</source>
        <translation>主界面连接：TCP 127.0.0.1:%1，从站地址 %2</translation>
    </message>
</context>
<context>
    <name>MainWindow</name>
    <message>
        <source>FieldLink - Industrial Modbus TCP/RTU Communication &amp; Data Acquisition Platform</source>
        <translation>FieldLink · 工业级Modbus TCP/RTU通信与数据采集平台</translation>
    </message>
    <message>
        <source>Modbus TCP/RTU · Industrial Communication · Data Acquisition</source>
        <translation>Modbus TCP/RTU · 工业通信 · 数据采集</translation>
    </message>
    <message>
        <source>● Offline</source>
        <translation>● 未连接</translation>
    </message>
    <message>
        <source>● Connected · %1</source>
        <translation>● 已连接 · %1</translation>
    </message>
    <message>
        <source>Connection type:</source>
        <translation>连接类型:</translation>
    </message>
    <message>
        <source>Serial</source>
        <translation>串口 RTU</translation>
    </message>
    <message>
        <source>Port / Host:</source>
        <translation>地址 / 串口:</translation>
    </message>
    <message>
        <source>Server Address:</source>
        <translation>从站地址:</translation>
    </message>
    <message>
        <source>Connect</source>
        <translation>连 接</translation>
    </message>
    <message>
        <source>Disconnect</source>
        <translation>断开</translation>
    </message>
    <message>
        <source>Read Area</source>
        <translation>实时读取</translation>
    </message>
    <message>
        <source>Write Area</source>
        <translation>数据写入</translation>
    </message>
    <message>
        <source>Start address:</source>
        <translation>起始地址:</translation>
    </message>
    <message>
        <source>Number of values:</source>
        <translation>寄存器数量:</translation>
    </message>
    <message>
        <source>Result:</source>
        <translation>读取结果:</translation>
    </message>
    <message>
        <source>Table:</source>
        <translation>寄存器表:</translation>
    </message>
    <message>
        <source>Read</source>
        <translation>读取</translation>
    </message>
    <message>
        <source>Write</source>
        <translation>写入</translation>
    </message>
    <message>
        <source>Read-Write</source>
        <translation>读 / 写</translation>
    </message>
    <message>
        <source>&amp;Device</source>
        <translation>设备(&amp;D)</translation>
    </message>
    <message>
        <source>Too&amp;ls</source>
        <translation>工具(&amp;T)</translation>
    </message>
    <message>
        <source>&amp;Connect</source>
        <translation>连接(&amp;C)</translation>
    </message>
    <message>
        <source>&amp;Disconnect</source>
        <translation>断开(&amp;D)</translation>
    </message>
    <message>
        <source>&amp;Quit</source>
        <translation>退出(&amp;Q)</translation>
    </message>
    <message>
        <source>&amp;Options</source>
        <translation>选项(&amp;O)</translation>
    </message>
    <message>
        <source>&amp;View</source>
        <translation>视图(&amp;V)</translation>
    </message>
    <message>
        <source>&amp;Data</source>
        <translation>数据(&amp;D)</translation>
    </message>
    <message>
        <source>&amp;Advanced</source>
        <translation>高级(&amp;A)</translation>
    </message>
    <message>
        <source>&amp;Profile</source>
        <translation>方案(&amp;P)</translation>
    </message>
    <message>
        <source>Log Viewer</source>
        <translation>日志查看器</translation>
    </message>
    <message>
        <source>Real-time Chart</source>
        <translation>实时曲线</translation>
    </message>
    <message>
        <source>Data Dashboard</source>
        <translation>数据仪表盘</translation>
    </message>
    <message>
        <source>Interface Theme</source>
        <translation>界面主题</translation>
    </message>
    <message>
        <source>Light Modern</source>
        <translation>浅色现代风</translation>
    </message>
    <message>
        <source>Dark Industrial</source>
        <translation>深色工业风</translation>
    </message>
    <message>
        <source>Language</source>
        <translation>语言 / Language</translation>
    </message>
    <message>
        <source>Export CSV</source>
        <translation>导出 CSV</translation>
    </message>
    <message>
        <source>Scheduled Polling</source>
        <translation>定时轮询</translation>
    </message>
    <message>
        <source>Batch Tasks</source>
        <translation>批量任务</translation>
    </message>
    <message>
        <source>Alarm Config</source>
        <translation>报警配置</translation>
    </message>
    <message>
        <source>History Query</source>
        <translation>历史查询</translation>
    </message>
    <message>
        <source>Device Manager</source>
        <translation>多设备管理</translation>
    </message>
    <message>
        <source>Device Templates</source>
        <translation>设备模板</translation>
    </message>
    <message>
        <source>Script Console</source>
        <translation>脚本控制台</translation>
    </message>
    <message>
        <source>Remote Service</source>
        <translation>远程服务</translation>
    </message>
    <message>
        <source>Plugin Manager</source>
        <translation>插件管理</translation>
    </message>
    <message>
        <source>Point Manager</source>
        <translation>点位管理</translation>
    </message>
    <message>
        <source>Verification Tests</source>
        <translation>验证测试</translation>
    </message>
    <message>
        <source>Product Delivery</source>
        <translation>产品交付</translation>
    </message>
    <message>
        <source>Save Profile</source>
        <translation>保存配置</translation>
    </message>
    <message>
        <source>Load Profile</source>
        <translation>加载配置</translation>
    </message>
    <message>
        <source>Ready | FieldLink industrial Modbus TCP/RTU communication and data acquisition platform</source>
        <translation>就绪 | FieldLink 工业级 Modbus TCP/RTU 通信与数据采集平台</translation>
    </message>
    <message>
        <source>Coils</source>
        <translation>线圈</translation>
    </message>
    <message>
        <source>Discrete Inputs</source>
        <translation>离散输入</translation>
    </message>
    <message>
        <source>Input Registers</source>
        <translation>输入寄存器</translation>
    </message>
    <message>
        <source>Holding Registers</source>
        <translation>保持寄存器</translation>
    </message>
    <message>
        <source>Could not create Modbus master.</source>
        <translation>无法创建 Modbus 主站（RTU）。</translation>
    </message>
    <message>
        <source>Could not create Modbus client.</source>
        <translation>无法创建 Modbus 客户端（TCP）。</translation>
    </message>
    <message>
        <source>Connect failed: </source>
        <translation>连接失败: </translation>
    </message>
    <message>
        <source>Read error: </source>
        <translation>读取错误: </translation>
    </message>
    <message>
        <source>Write error: </source>
        <translation>写入错误: </translation>
    </message>
    <message>
        <source>Address: %1, Value: %2</source>
        <translation>地址: %1, 数值: %2</translation>
    </message>
    <message>
        <source>Read response error: %1 (Mobus exception: 0x%2)</source>
        <translation>读取响应错误: %1 (Modbus 异常: 0x%2)</translation>
    </message>
    <message>
        <source>Read response error: %1 (code: 0x%2)</source>
        <translation>读取响应错误: %1 (代码: 0x%2)</translation>
    </message>
    <message>
        <source>Write response error: %1 (Mobus exception: 0x%2)</source>
        <translation>写入响应错误: %1 (Modbus 异常: 0x%2)</translation>
    </message>
    <message>
        <source>Write response error: %1 (code: 0x%2)</source>
        <translation>写入响应错误: %1 (代码: 0x%2)</translation>
    </message>
    <message>
        <source>FieldLink 采集平台</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>TCP</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>1</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>2</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>3</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>4</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>5</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>6</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>7</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>8</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>9</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>10</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>MQTT Publishing</source>
        <translation>MQTT 发布</translation>
    </message>
    <message>
        <source>Simulator (Modbus Slave)</source>
        <translation>模拟设备（Modbus 从站）</translation>
    </message>
    <message>
        <source>MQTT Publish Settings</source>
        <translation>MQTT 发布设置</translation>
    </message>
    <message>
        <source>Leave empty to auto-generate</source>
        <translation>留空自动生成</translation>
    </message>
    <message>
        <source> s</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Broker address</source>
        <translation>Broker 地址</translation>
    </message>
    <message>
        <source>Port</source>
        <translation>端口</translation>
    </message>
    <message>
        <source>ClientID</source>
        <translation>ClientID</translation>
    </message>
    <message>
        <source>Username (optional)</source>
        <translation>用户名（可选）</translation>
    </message>
    <message>
        <source>Password (optional)</source>
        <translation>密码（可选）</translation>
    </message>
    <message>
        <source>Topic prefix</source>
        <translation>主题前缀</translation>
    </message>
    <message>
        <source>KeepAlive</source>
        <translation>KeepAlive</translation>
    </message>
    <message>
        <source>Connected to %1</source>
        <translation>已连接 %1</translation>
    </message>
    <message>
        <source>Not connected</source>
        <translation>未连接</translation>
    </message>
    <message>
        <source>Status</source>
        <translation>状态</translation>
    </message>
    <message>
        <source>Save and connect</source>
        <translation>保存并连接</translation>
    </message>
    <message>
        <source>Connecting...</source>
        <translation>正在连接...</translation>
    </message>
    <message>
        <source>Disconnected</source>
        <translation>已断开</translation>
    </message>
    <message>
        <source>User Login</source>
        <translation>用户登录</translation>
    </message>
    <message>
        <source>Username</source>
        <translation>用户名</translation>
    </message>
    <message>
        <source>Password</source>
        <translation>密码</translation>
    </message>
    <message>
        <source>Login Failed</source>
        <translation>登录失败</translation>
    </message>
    <message>
        <source>Wrong username or password. Sensitive operations will be denied.</source>
        <translation>用户名或密码错误，敏感操作将被拒绝</translation>
    </message>
    <message>
        <source>Change Password</source>
        <translation>修改密码</translation>
    </message>
    <message>
        <source>This account uses the default/initial password. Set a new one (min 6 chars):</source>
        <translation>当前账号使用默认/初始密码，请设置新密码（至少 6 位）：</translation>
    </message>
    <message>
        <source>Password Too Short</source>
        <translation>密码过短</translation>
    </message>
    <message>
        <source>Password must be at least 6 characters</source>
        <translation>密码至少需要 6 个字符</translation>
    </message>
    <message>
        <source>Confirm New Password</source>
        <translation>确认新密码</translation>
    </message>
    <message>
        <source>Please re-enter the new password:</source>
        <translation>请再次输入新密码：</translation>
    </message>
    <message>
        <source>Mismatch</source>
        <translation>两次输入不一致</translation>
    </message>
    <message>
        <source>Passwords do not match. Please try again.</source>
        <translation>两次输入的密码不一致，请重试</translation>
    </message>
    <message>
        <source>Password Not Changed</source>
        <translation>未修改密码</translation>
    </message>
    <message>
        <source>For security, the default/initial password must be changed before use. This login was denied.</source>
        <translation>出于安全考虑，使用默认/初始密码必须先完成修改，本次登录已被拒绝</translation>
    </message>
    <message>
        <source>Password Changed</source>
        <translation>密码已修改</translation>
    </message>
    <message>
        <source>Remember your new password for the next login.</source>
        <translation>请牢记新密码，下次登录使用</translation>
    </message>
    <message>
        <source>Auto-reconnect stopped</source>
        <translation>已取消自动重连</translation>
    </message>
    <message>
        <source>Connect Failed</source>
        <translation>连接失败</translation>
    </message>
    <message>
        <source>Connect failed: %1</source>
        <translation>连接失败: %1</translation>
    </message>
    <message>
        <source>Holding register: double-click a cell to enter a hex value, then press Write.</source>
        <translation>保持寄存器：双击单元格输入十六进制值，然后点「写入」下发</translation>
    </message>
    <message>
        <source>Coil: check = write 1 (ON), uncheck = write 0 (OFF), then press Write.</source>
        <translation>线圈：勾选 = 写 1（ON），取消勾选 = 写 0（OFF），然后点「写入」下发</translation>
    </message>
    <message>
        <source>This register type is read-only and cannot be written.</source>
        <translation>该寄存器类型只读，不支持写入</translation>
    </message>
    <message>
        <source>Auto read</source>
        <translation>定时读取</translation>
    </message>
    <message>
        <source>Interval</source>
        <translation>间隔</translation>
    </message>
    <message>
        <source> ms</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Connect first before enabling auto read</source>
        <translation>请先连接设备再开启定时读取</translation>
    </message>
    <message>
        <source>Connection Closed</source>
        <translation>连接已断开</translation>
    </message>
    <message>
        <source>The server has closed the connection (%1).

Do you want to reconnect?</source>
        <translation>服务器已主动断开连接（%1）。

是否重新连接？</translation>
    </message>
    <message>
        <source>Reconnect declined; press Connect to dial again</source>
        <translation>已取消自动重连：请点击「Connect」手动重连</translation>
    </message>
    <message>
        <source>Auto reading every %1 ms (uncheck to stop)</source>
        <translation>自动读取中：每 %1 ms（取消勾选停止）</translation>
    </message>
    <message>
        <source>● Auto reading every %1 ms (uncheck to stop) — last update %2</source>
        <translation>● 自动读取中：每 %1 ms（取消勾选停止）— 最后更新 %2</translation>
    </message>
</context>
<context>
    <name>MqttClient</name>
    <message>
        <source>MQTT broker address is not configured</source>
        <translation>MQTT broker 地址未配置</translation>
    </message>
    <message>
        <source>MQTT connection error: %1</source>
        <translation>MQTT 连接错误: %1</translation>
    </message>
    <message>
        <source>MQTT reconnecting to %1 ...</source>
        <translation>MQTT 正在重连 %1 ...</translation>
    </message>
    <message>
        <source>Connection accepted</source>
        <translation>连接被接受</translation>
    </message>
    <message>
        <source>Unsupported protocol version</source>
        <translation>不支持的协议版本</translation>
    </message>
    <message>
        <source>Client identifier rejected</source>
        <translation>客户端标识符被拒绝</translation>
    </message>
    <message>
        <source>Server unavailable</source>
        <translation>服务端不可用</translation>
    </message>
    <message>
        <source>Bad username or password</source>
        <translation>用户名或密码错误</translation>
    </message>
    <message>
        <source>Not authorized</source>
        <translation>未授权</translation>
    </message>
    <message>
        <source>Unknown return code %1</source>
        <translation>未知返回码 %1</translation>
    </message>
    <message>
        <source>MQTT not connected: dropping published messages (%1 dropped, resumes when connected)</source>
        <translation>MQTT 未连接，已丢弃 %1 条上送消息，恢复连接后自动继续</translation>
    </message>
    <message>
        <source>MQTT broker refused connection: %1</source>
        <translation>MQTT broker 拒绝连接: %1</translation>
    </message>
</context>
<context>
    <name>SettingsDialog</name>
    <message>
        <source>Modbus Settings</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Serial Parameters</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Parity:</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>No</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Even</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Odd</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Space</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Mark</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Baud Rate:</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>1200</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>2400</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>4800</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>9600</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>19200</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>38400</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>57600</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>115200</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Data Bits:</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>5</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>6</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>7</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>8</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Stop Bits:</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>1</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>3</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>2</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Response Timeout:</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source> ms</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Number of retries:</source>
        <translation type="unfinished" />
    </message>
    <message>
        <source>Apply</source>
        <translation type="unfinished" />
    </message>
</context>
<context>
    <name>WriteRegisterModel</name>
    <message>
        <source>Coil (check = ON)</source>
        <translation>线圈（勾选 = ON）</translation>
    </message>
    <message>
        <source>Holding register (hex)</source>
        <translation>保持寄存器（十六进制）</translation>
    </message>
</context>
</TS>