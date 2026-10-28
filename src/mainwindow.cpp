
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "settingsdialog.h"
#include "writeregistermodel.h"
#include "logviewer.h"
#include "pollmanager.h"
#include "dataexporter.h"
#include "realtimechart.h"
#include "devicemanager.h"
#include "devicetemplate.h"
#include "scriptengine.h"
#include "remoteserver.h"
#include "historydata.h"
#include "pluginmanager.h"
#include "alarmmanager.h"
#include "alarmrepository.h"
#include "batchtaskmanager.h"
#include "dashboard.h"
#include "configprofile.h"
#include "reliabilitymanager.h"
#include "securitymanager.h"
#include "pointmodel.h"
#include "verificationmanager.h"
#include "deliverymanager.h"
#include "thememanager.h"  // 深色/浅色主题切换
#include "mqttclient.h"    // MQTT 发布端客户端

#include <QModbusTcpClient>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtSerialBus/qmodbusrtuserialclient.h>
#else
// Qt 5 中该类名为 QModbusRtuSerialMaster，Qt 6.2 起才更名为 QModbusRtuSerialClient
#include <QtSerialBus/qmodbusrtuserialmaster.h>
using QModbusRtuSerialClient = QModbusRtuSerialMaster;
#endif
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QLabel>
#include <QDateTime>
#include <QTime>
#include <QUrl>
#include <QIcon>
#include <QSerialPortInfo>
#include <QDateTime>
#include <QDate>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QCoreApplication>
#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QListWidgetItem>
#include <QAbstractItemView>
#include <QFileInfo>
#include <QColor>
#include <QJsonObject>
#include <QJsonArray>
#include <QSizePolicy>
#include <QHeaderView>
#include <QStyle>
#include <QLocale>
#include <QTimer>
#include <QCheckBox>
#include <QSpinBox>

enum ModbusConnection {
    Serial,
    Tcp
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , lastRequest(nullptr)
    , modbusDevice(nullptr)
{
    ui->setupUi(this);
    ui->portEdit->setMinimumWidth(240);
    ui->portEdit->setMinimumContentsLength(22);
    ui->portEdit->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);

    // 实时读取：定时轮询（勾选「Auto read」后按间隔自动执行读取）
    m_autoReadTimer = new QTimer(this);
    m_autoReadTimer->setInterval(ui->autoReadInterval->value());
    connect(m_autoReadTimer, &QTimer::timeout, this, &MainWindow::onAutoReadTimeout);
    connect(ui->autoReadCheck, &QCheckBox::toggled, this, &MainWindow::onAutoReadToggled);
    connect(ui->autoReadInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_autoReadTimer->isActive())
            m_autoReadTimer->setInterval(value);
        if (m_autoReadIndicator && m_autoReadIndicator->isVisible())
            updateAutoReadIndicator(true);   // 运行中间隔变化时同步指示文案
    });

    // 定时读取常驻指示：状态栏右侧红点标签，比底部小字更醒目
    m_autoReadIndicator = new QLabel(this);
    m_autoReadIndicator->setStyleSheet(QStringLiteral("color:#e74c3c; font-weight:bold; padding:0 8px;"));
    m_autoReadIndicator->hide();
    statusBar()->addPermanentWidget(m_autoReadIndicator);

    // 「定时读取」勾选框做成醒目的胶囊开关：平时灰边框，勾选后绿底白字
    ui->autoReadCheck->setStyleSheet(QStringLiteral(
        "QCheckBox{font-weight:bold; font-size:13px; padding:4px 12px;"
        "  border:2px solid #95a5a6; border-radius:14px;}"
        "QCheckBox:hover{border-color:#27ae60;}"
        "QCheckBox:checked{color:white; background-color:#27ae60; border-color:#27ae60;}"));

    ui->readValue->setAlternatingRowColors(true);
    ui->writeValueTable->setAlternatingRowColors(true);
    ui->writeValueTable->setRootIsDecorated(false);
    ui->writeValueTable->setUniformRowHeights(true);
    ui->writeValueTable->header()->setStretchLastSection(true);
    // 设置窗口标题和图标（标题走翻译系统，见 fieldlink_zh_CN.ts）
    setWindowTitle(tr("FieldLink - Industrial Modbus TCP/RTU Communication & Data Acquisition Platform"));
    setWindowIcon(QIcon(":/images/logo.ico")); // 需要添加资源文件

    // ---------- 界面语言与状态胶囊初始化 ----------
    // 翻译资源经 qmake 的 CONFIG += lrelease embed_translations 嵌入 :/i18n
    m_translator = new QTranslator(this);
    applyLanguage(m_appSettings.value(QStringLiteral("ui/language"),
        QLocale::system().name()).toString());
    updateConnectionChip(false);

    m_settingsDialog = new SettingsDialog(this);

    initActions();

    writeModel = new WriteRegisterModel(this);
    writeModel->setStartAddress(ui->writeAddress->value());
    writeModel->setNumberOfValues(ui->writeSize->currentText());

    ui->writeValueTable->setModel(writeModel);
    ui->writeValueTable->hideColumn(2);
    connect(writeModel, &WriteRegisterModel::updateViewport, ui->writeValueTable->viewport(),
        static_cast<void (QWidget::*)()>(&QWidget::update));

    ui->writeTable->addItem(tr("Coils"), QModbusDataUnit::Coils);
    ui->writeTable->addItem(tr("Discrete Inputs"), QModbusDataUnit::DiscreteInputs);
    ui->writeTable->addItem(tr("Input Registers"), QModbusDataUnit::InputRegisters);
    ui->writeTable->addItem(tr("Holding Registers"), QModbusDataUnit::HoldingRegisters);

    ui->connectType->setCurrentIndex(0);
    on_connectType_currentIndexChanged(0);

    auto model = new QStandardItemModel(10, 1, this);
    for (int i = 0; i < 10; ++i)
        model->setItem(i, new QStandardItem(QStringLiteral("%1").arg(i + 1)));
    ui->writeSize->setModel(model);
    ui->writeSize->setCurrentText("10");
    connect(ui->writeSize,&QComboBox::currentTextChanged, writeModel,
        &WriteRegisterModel::setNumberOfValues);

    auto valueChanged = static_cast<void (QSpinBox::*)(int)> (&QSpinBox::valueChanged);
    connect(ui->writeAddress, valueChanged, writeModel, &WriteRegisterModel::setStartAddress);


    // 设置最小窗口尺寸
    setMinimumSize(1100, 750);

    // 初始化高级功能
    initAdvancedFeatures();
    initMenus();

    // 串口枚举与设置恢复
    populateSerialPorts();
    loadSettings();



}

MainWindow::~MainWindow()
{
    if (m_pollManager)
        m_pollManager->stopAll();
    if (m_remoteServer)
        m_remoteServer->stop();
    if (m_reliabilityManager)
        m_reliabilityManager->stop();
    if (m_securityManager)
        m_securityManager->save(m_appSettings);
    if (m_pointModel)
        m_pointModel->saveToFile(QCoreApplication::applicationDirPath() + "/points.json");
    if (m_deviceManager)
        m_deviceManager->saveToSettings(m_appSettings);
    if (m_templateManager)
        m_templateManager->saveToFile(QCoreApplication::applicationDirPath() + "/templates.json");
    m_appSettings.sync();

    // 关闭标志：置位后所有回调立即返回。
    // delete modbusDevice 会触发在途 QModbusReply 的 finished() →
    // readReady() → PointModel::pointValueUpdated → lambda 里 m_chart->addDataPoint()。
    // 若此时 m_chart 已被置空 → 段错误。析构期间必须先切断回调链。
    m_shuttingDown = true;

    if (modbusDevice)
        modbusDevice->disconnectDevice();
    delete modbusDevice;
    modbusDevice = nullptr;

    delete m_chart;
    m_chart = nullptr;
    delete m_dashboard;
    m_dashboard = nullptr;

    saveSettings();

    // 报警仓库是 MainWindow 的成员，析构顺序上先于 QObject 子对象（AlarmManager）。
    // 先解除引用，避免析构期间 AlarmManager 再访问已销毁的仓库。
    if (m_alarmManager)
        m_alarmManager->setRepository(nullptr);
    m_alarmRepository.reset();

    delete ui;
}

void MainWindow::initActions()
{
    ui->actionConnect->setEnabled(true);
    ui->actionDisconnect->setEnabled(false);
    ui->actionExit->setEnabled(true);
    ui->actionOptions->setEnabled(true);

    connect(ui->actionConnect, &QAction::triggered,
            this, &MainWindow::on_connectButton_clicked);
    connect(ui->actionDisconnect, &QAction::triggered,
            this, &MainWindow::on_connectButton_clicked);

    connect(ui->actionExit, &QAction::triggered, this, &QMainWindow::close);
    connect(ui->actionOptions, &QAction::triggered, m_settingsDialog, &QDialog::show);
}

void MainWindow::on_connectType_currentIndexChanged(int index)
{
    if (modbusDevice) {
        modbusDevice->disconnectDevice();
        delete modbusDevice;
        modbusDevice = nullptr;
    }

    auto type = static_cast<ModbusConnection> (index);
    if (type == Serial) {
        modbusDevice = new QModbusRtuSerialClient(this);
    } else if (type == Tcp) {
        modbusDevice = new QModbusTcpClient(this);
        if (ui->portEdit->currentText().isEmpty())
            ui->portEdit->setEditText(QStringLiteral("127.0.0.1:502"));
    }

    connect(modbusDevice, &QModbusClient::errorOccurred, [this](QModbusDevice::Error) {
        statusBar()->showMessage(modbusDevice->errorString(), 5000);
    });

    if (!modbusDevice) {
        ui->connectButton->setDisabled(true);
        if (type == Serial)
            statusBar()->showMessage(tr("Could not create Modbus master."), 5000);
        else
            statusBar()->showMessage(tr("Could not create Modbus client."), 5000);
    } else {
        connect(modbusDevice, &QModbusClient::stateChanged,
                this, &MainWindow::onStateChanged);
    }
}

void MainWindow::on_connectButton_clicked()
{
    if (!modbusDevice)
        return;

    statusBar()->clearMessage();
    if (modbusDevice->state() != QModbusDevice::ConnectedState) {
        // 已处于自动重试循环时再点一次 = 取消自动重连（给操作员停止手段）
        if (m_reliabilityManager->userIntentConnected()) {
            m_reliabilityManager->setUserIntentConnected(false);
            statusBar()->showMessage(tr("Auto-reconnect stopped"), 3000);
            logMessage(tr("Automatic reconnect cancelled"), 2);
            return;
        }
        // 用户明确要求连接：置位意图，意外断线时才会自动重连
        m_reliabilityManager->setUserIntentConnected(true);
        attemptModbusConnection(true);
    } else {
        // 用户主动断开：清除意图，自动重连循环立即停止
        m_userDisconnecting = true;   // 抑制 onStateChanged 中的"对端断开"弹窗
        m_reliabilityManager->setUserIntentConnected(false);
        modbusDevice->disconnectDevice();
        ui->actionConnect->setEnabled(true);
        ui->actionDisconnect->setEnabled(false);
        logMessage(tr("Disconnected"));
    }
}

// 执行一次连接动作（参数装配 + connectDevice）。
//   manual=true  : 操作员点击触发；失败弹窗提示
//   manual=false : 自动重连触发；失败仅状态栏提示（避免弹窗刷屏）
void MainWindow::attemptModbusConnection(bool manual)
{
    if (!modbusDevice)
        return;
    if (static_cast<ModbusConnection> (ui->connectType->currentIndex()) == Serial) {
        modbusDevice->setConnectionParameter(QModbusDevice::SerialPortNameParameter,
            ui->portEdit->currentText());
        modbusDevice->setConnectionParameter(QModbusDevice::SerialParityParameter,
            m_settingsDialog->settings().parity);
        modbusDevice->setConnectionParameter(QModbusDevice::SerialBaudRateParameter,
            m_settingsDialog->settings().baud);
        modbusDevice->setConnectionParameter(QModbusDevice::SerialDataBitsParameter,
            m_settingsDialog->settings().dataBits);
        modbusDevice->setConnectionParameter(QModbusDevice::SerialStopBitsParameter,
            m_settingsDialog->settings().stopBits);
    } else {
        const QUrl url = QUrl::fromUserInput(ui->portEdit->currentText());
        modbusDevice->setConnectionParameter(QModbusDevice::NetworkPortParameter, url.port());
        modbusDevice->setConnectionParameter(QModbusDevice::NetworkAddressParameter, url.host());
    }
    modbusDevice->setTimeout(m_settingsDialog->settings().responseTime);
    modbusDevice->setNumberOfRetries(m_settingsDialog->settings().numberOfRetries);
    if (!modbusDevice->connectDevice()) {
        // 按钮文字回滚 + 失败提示（手动连接失败弹窗，自动重连仅状态栏）
        ui->connectButton->setText(tr("Connect"));
        if (manual) {
            QMessageBox::warning(this, tr("Connect Failed"),
                                 tr("Connect failed: %1").arg(modbusDevice->errorString()));
        }
        statusBar()->showMessage(tr("Connect failed: ") + modbusDevice->errorString(), 5000);
    } else {
        ui->actionConnect->setEnabled(false);
        ui->actionDisconnect->setEnabled(true);
        ui->connectButton->setText(tr("Disconnect"));
        // 记录连接成功日志
        logMessage(tr("Connected: %1").arg(ui->portEdit->currentText()));
    }
}

void MainWindow::onStateChanged(int state)
{
    // 只有真正处于 ConnectedState 才算"已连接"。
    // 之前用 `state != UnconnectedState`，把 Connecting / Closing 也当成了已连接：
    //   - 断开过程中状态胶囊仍显示绿色、断开按钮仍可用；
    //   - ConnectingState 也会调用 notifySuccess()，把连续失败计数清零，
    //     导致 ReliabilityManager 的「连续通信失败 N 次」告警永远不会触发。
    const bool connected = (state == QModbusDevice::ConnectedState);
    const bool connecting = (state == QModbusDevice::ConnectingState);
    // 上一次状态（保留用于调试与兼容判断）
    m_lastModbusState = state;

    // "曾真正连上过"标志：独立于状态序列维护。
    // 不能依赖 m_lastModbusState == ConnectedState —— 断开时 Qt 会先发
    // ClosingState 再发 UnconnectedState，前者会把"曾连接过"的信息冲掉。
    if (connected)
        m_hadRealConnection = true;

    ui->actionConnect->setEnabled(!connected && !connecting);
    ui->actionDisconnect->setEnabled(connected);

    // 设备断开时自动取消「定时读取」，避免空转报错
    if (!connected && ui->autoReadCheck->isChecked())
        ui->autoReadCheck->setChecked(false);

    if (state == QModbusDevice::UnconnectedState)
        ui->connectButton->setText(tr("Connect"));
    else if (state == QModbusDevice::ConnectedState)
        ui->connectButton->setText(tr("Disconnect"));

    updateConnectionChip(connected);

    // MQTT：设备连接状态变化时发布保留消息（上位机/网关可据此判断现场在线）
    publishMqttStatus(connected);

    if (m_dashboard) {
        m_dashboard->setConnectionStatus(connected,
            QStringLiteral("%1 %2").arg(connected ? tr("Connected") : tr("Disconnected"), ui->portEdit->currentText()));
        m_dashboard->setServerAddress(ui->serverEdit->value());
    }
    if (m_reliabilityManager) {
        if (connected) {
            // 只有真正连上才记成功
            m_reliabilityManager->notifySuccess();
        } else if (state == QModbusDevice::UnconnectedState) {
            // 只有真正断开（UnconnectedState）才算一次通信失败。
            // ClosingState（正在断开）不算 —— 否则一次断线会被计两次失败。
            // 意外断线（此前确实连上过且非用户主动断开）＝对端主动断开/网络中断：
            // 先暂停自动重连循环，弹窗交给用户决定是否重连
            const bool unexpected = m_hadRealConnection && !m_userDisconnecting;
            if (unexpected)
                m_reliabilityManager->setUserIntentConnected(false);
            m_reliabilityManager->notifyDisconnected();
            if (unexpected)
                askReconnectAfterRemoteClose();
            // 主连接断开：主连接下的点位立即降级，避免界面继续显示"最后一次 Good 值"
            if (m_hadRealConnection && m_pointModel)
                m_pointModel->markDeviceBad(0, DataQuality::Bad,
                                            QStringLiteral("main link disconnected"));
            m_hadRealConnection = false;   // 本轮断开流程已处理完毕
            m_userDisconnecting = false;   // 恢复默认
        }
    }
    saveSettings();
}

// 对端主动断开后的询问弹窗：
//   「是」→ 立即重连并恢复自动重连循环（再次被对端断开时仍会询问）
//   「否」→ 不再自动重连，直到用户手动点击「Connect」发起连接
void MainWindow::askReconnectAfterRemoteClose()
{
    const QString addr = ui->portEdit->currentText();
    logMessage(tr("Device connection closed by peer: %1").arg(addr), 2);

    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, tr("Connection Closed"),
        tr("The server has closed the connection (%1).\n\nDo you want to reconnect?").arg(addr),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (choice == QMessageBox::Yes) {
        logMessage(tr("User chose to reconnect"));
        m_reliabilityManager->setUserIntentConnected(true);
        attemptModbusConnection(false);   // 失败仅状态栏提示，自动重连循环接管后续重试
        if (modbusDevice && modbusDevice->state() == QModbusDevice::UnconnectedState)
            m_reliabilityManager->notifyDisconnected();   // 立即失败时兜底启动重连循环
    } else {
        logMessage(tr("User chose not to reconnect; waiting for manual connection"), 2);
        statusBar()->showMessage(tr("Reconnect declined; press Connect to dial again"), 5000);
    }
}

void MainWindow::on_readButton_clicked()
{
    if (!modbusDevice)
        return;
    statusBar()->clearMessage();
    sendReadRequests(true);   // 手动读取：先清空结果列表（刷新模式随请求下发）
}

/** @brief 发起分段读取
 *  @param clearResult true  : 手动读取语义，发送前清空「读取结果」列表
 *  @param clearResult false : 定时读取语义，滚动追加，值变化肉眼可见且不闪屏
 *  @return 返回是否成功发出了至少一个请求
 */
bool MainWindow::sendReadRequests(bool clearResult)
{
    if (!modbusDevice)
        return false;
    if (clearResult) {
        ui->readValue->clear();
        m_readRowsByAddr.clear();   // 列表被清空，地址→行映射必须同步清空
    }

    // 分段读取
    const auto req = readRequest();
    // 构造分段（函数在后文定义）
    auto makeReadSegments = [](QModbusDataUnit::RegisterType type, int start, int count){
        QList<QModbusDataUnit> segs; int limit = 0;
        switch (type) {
        case QModbusDataUnit::Coils:
        case QModbusDataUnit::DiscreteInputs: limit = 2000; break; // bits
        case QModbusDataUnit::InputRegisters:
        case QModbusDataUnit::HoldingRegisters:
        default: limit = 125; break; // 16-bit
        }
        int remaining = count, addr = start;
        while (remaining > 0) {
            const int take = qMin(remaining, limit);
            segs.append(QModbusDataUnit(type, addr, take));
            addr += take; remaining -= take;
        }
        return segs;
    };

    const auto segments = makeReadSegments(req.registerType(), req.startAddress(), req.valueCount());
    bool sent = false;
    for (const auto &seg : segments) {
        logMessage(QStringLiteral("READ[%1] addr=%2 count=%3")
                   .arg(seg.registerType()).arg(seg.startAddress()).arg(seg.valueCount()));
        if (auto *reply = modbusDevice->sendReadRequest(seg, ui->serverEdit->value())) {
            sent = true;
            if (!reply->isFinished()) {
                // 刷新模式必须随请求走，不能用共享成员标志：
                // 标志在「发送时写入、回包时读取」，而回包是异步的，
                // 期间可能被另一次请求改写 —— 定时读与手动读并发时模式串扰，
                // 导致定时读被当成手动读而不断追加行（列表无限膨胀）。
                reply->setProperty(kReadInPlaceProperty, !clearResult);
                ++m_pendingReadRequests;
                connect(reply, &QModbusReply::finished, this, &MainWindow::readReady);
            } else {
                delete reply; // 广播回复会立即返回
            }
        } else {
            statusBar()->showMessage(tr("Read error: ") + modbusDevice->errorString(), 5000);
        }
    }
    return sent;
}

void MainWindow::readReady()
{
    auto reply = qobject_cast<QModbusReply *>(sender());
    if (!reply)
        return;

    // 取出该请求自己的刷新模式（发送时随 reply 一起保存）
    const bool inPlace = reply->property(kReadInPlaceProperty).toBool();

    if (reply->error() == QModbusDevice::NoError) {
        const QModbusDataUnit unit = reply->result();
        ui->readValue->setUpdatesEnabled(false);
        QListWidgetItem *lastItem = nullptr;
        QVector<quint16> raw;
        raw.reserve(static_cast<int>(unit.valueCount()));
        // 本次响应覆盖的地址集合：用于清理上一轮遗留的、不再被读取的旧行
        QSet<int> seenAddrs;
        for (uint i = 0; i < unit.valueCount(); i++) {
            const int addr = unit.startAddress() + static_cast<int>(i);
            const quint16 val = unit.value(i);
            const QString entry = tr("Address: %1, Value: %2").arg(addr)
                                     .arg(QString::number(val,
                                          unit.registerType() <= QModbusDataUnit::Coils ? 10 : 16));
            raw.append(val);
            seenAddrs.insert(addr);

            QListWidgetItem *item = nullptr;
            if (inPlace) {
                // 定时读取：按地址匹配既有行，原地刷新数值，不追加新记录。
                // 用哈希表做地址→行 映射：线性查找是 O(值数 × 行数)，
                // 读 125 个值且列表 500 行时每 tick 6 万次比较，位区 2000 个线圈时更甚。
                item = m_readRowsByAddr.value(addr, nullptr);
                if (item) {
                    item->setText(entry);
                    continue;   // 行数与顺序保持不变
                }
                // 首次读到该地址：先追加一行，之后一直原地刷新
            }

            item = new QListWidgetItem(entry);
            item->setData(Qt::UserRole, addr);
            ui->readValue->addItem(item);
            if (inPlace)
                m_readRowsByAddr.insert(addr, item);
            lastItem = item;
        }

        // 清理本轮未覆盖的旧行：用户改了读取起始地址后，旧地址的行会永久滞留，
        // 列表只增不减最终膨胀到无法使用。
        if (inPlace && !m_readRowsByAddr.isEmpty()) {
            const QList<int> stale = m_readRowsByAddr.keys().toList();
            for (int addr : stale) {
                if (seenAddrs.contains(addr))
                    continue;
                QListWidgetItem *row = m_readRowsByAddr.take(addr);
                if (row) {
                    const int rowIndex = ui->readValue->row(row);
                    if (rowIndex >= 0)
                        ui->readValue->takeItem(rowIndex);
                }
            }
        }

        ui->readValue->setUpdatesEnabled(true);
        if (lastItem)
            ui->readValue->scrollToItem(lastItem, QAbstractItemView::PositionAtBottom);
        logMessage(QStringLiteral("READ-OK addr=%1 count=%2").arg(unit.startAddress()).arg(unit.valueCount()));

        // 同步驱动可视化/存档/报警：让手动读、定时读取与轮询路径行为一致。
        // 此前只有轮询任务会喂点位模型/历史库/报警，定时读取读到的值
        // 不会反映到曲线/仪表盘，也不会触发报警规则。
        const int server = ui->serverEdit->value();
        if (m_pointModel && !raw.isEmpty())
            m_pointModel->updateFromRaw(server, unit.registerType(), unit.startAddress(), raw);
        if (m_historyData && m_historyData->isOpen() && !raw.isEmpty())
            m_historyData->addRecord(server, unit.registerType(), unit.startAddress(), raw);
        if (m_dataExporter && !raw.isEmpty()) {
            ExportRecord rec;
            rec.timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
            rec.serverAddress = server;
            rec.registerType = unit.registerType();
            rec.startAddress = unit.startAddress();
            rec.values = raw;
            m_dataExporter->addRecord(rec);
        }
        if (m_alarmManager) {
            for (int i = 0; i < raw.size(); ++i)
                m_alarmManager->checkValue(server, static_cast<int>(unit.registerType()),
                                           unit.startAddress() + i, raw[i]);
        }
        // 定时读取运行中：指示标签显示最后成功更新时间，方便确认"确实在读"
        if (m_autoReadTimer->isActive() && m_autoReadIndicator)
            m_autoReadIndicator->setText(tr("● Auto reading every %1 ms (uncheck to stop) — last update %2")
                                             .arg(ui->autoReadInterval->value())
                                             .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss"))));
    } else if (reply->error() == QModbusDevice::ProtocolError) {
        statusBar()->showMessage(tr("Read response error: %1 (Mobus exception: 0x%2)").
                                    arg(reply->errorString()).
                                    arg(reply->rawResult().exceptionCode(), -1, 16), 5000);
        logMessage(QStringLiteral("READ-ERR protocol=%1 code=0x%2").arg(reply->errorString()).arg(reply->rawResult().exceptionCode(), -1, 16));
    } else {
        statusBar()->showMessage(tr("Read response error: %1 (code: 0x%2)").
                                    arg(reply->errorString()).
                                    arg(reply->error(), -1, 16), 5000);
        logMessage(QStringLiteral("READ-ERR %1 code=0x%2").arg(reply->errorString()).arg(reply->error(), -1, 16));
    }

    // 在途计数递减：分段读时第一段回包就清 busy 会让下一 tick 提前发请求，
    // 其余分段仍在途 → 请求堆积。用计数而非布尔量才准确。
    if (m_pendingReadRequests > 0)
        --m_pendingReadRequests;
    m_autoReadBusy = (m_pendingReadRequests > 0);
    reply->deleteLater();
}

// 实时读取：定时轮询开关。设备未连接时不允许开启。
void MainWindow::onAutoReadToggled(bool checked)
{
    if (checked) {
        if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState) {
            ui->autoReadCheck->setChecked(false);
            statusBar()->showMessage(tr("Connect first before enabling auto read"), 3000);
            return;
        }
        m_autoReadBusy = false;
        m_autoReadTimer->setInterval(ui->autoReadInterval->value());
        m_autoReadTimer->start();
        updateAutoReadIndicator(true);
        statusBar()->showMessage(tr("Auto reading every %1 ms (uncheck to stop)")
                                     .arg(ui->autoReadInterval->value()), 4000);
        logMessage(tr("Timed reading started: every %1 ms").arg(ui->autoReadInterval->value()));
        // 立即触发第一次读取，不用干等一个周期就能看到值刷新
        QTimer::singleShot(0, this, &MainWindow::onAutoReadTimeout);
    } else {
        m_autoReadTimer->stop();
        m_autoReadBusy = false;
        updateAutoReadIndicator(false);
        logMessage(tr("Timed reading stopped"));
    }
}

// 定时读取状态指示：状态栏常驻红点标签，一眼可见是否在轮询
void MainWindow::updateAutoReadIndicator(bool on)
{
    if (!m_autoReadIndicator)
        return;
    if (on) {
        m_autoReadIndicator->setText(tr("● Auto reading every %1 ms (uncheck to stop)")
                                         .arg(ui->autoReadInterval->value()));
        m_autoReadIndicator->show();
    } else {
        m_autoReadIndicator->hide();
    }
}

// 定时到点：发起一次读取。带在途保护——上一 tick 未完成时跳过本次，
// 避免短间隔下请求堆积导致响应越来越慢甚至失败。
void MainWindow::onAutoReadTimeout()
{
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState) {
        ui->autoReadCheck->setChecked(false);
        return;
    }
    if (m_autoReadBusy || m_pendingReadRequests > 0)
        return;   // 在途保护：跳过本 tick

    // 定时读取：按地址原地刷新既有行，不追加新记录
    // （刷新模式随请求下发，见 sendReadRequests 中 reply->setProperty）
    if (!sendReadRequests(false))
        m_autoReadBusy = false;   // 一条都没发出去（如设备繁忙），下一 tick 重试
    else
        m_autoReadBusy = true;    // readReady 回调里按在途计数复位
}

void MainWindow::on_writeButton_clicked()
{
    if (!modbusDevice)
        return;
    statusBar()->clearMessage();
    if (m_securityManager)
        m_securityManager->audit(QStringLiteral("local"), QStringLiteral("WRITE"),
            QStringLiteral("server=%1 addr=%2 count=%3")
                .arg(ui->serverEdit->value()).arg(ui->writeAddress->value()).arg(ui->writeSize->currentText()));

    QModbusDataUnit writeUnit = writeRequest();
    QModbusDataUnit::RegisterType table = writeUnit.registerType();

    // 越界保护：writeAddress 最大 65535、writeSize 是可编辑下拉框（用户可输入任意数量），
    // 二者之和可能超出 65536 的地址空间。此前直接按 i + startAddress 索引 65536 大小的
    // 缓存 → Release 构建（Q_ASSERT 被编译掉）静默越界读，把堆外垃圾值写给从站。
    const int start = writeUnit.startAddress();
    const int count = static_cast<int>(writeUnit.valueCount());
    if (count <= 0 || start < 0 || start + count > 65536) {
        statusBar()->showMessage(tr("Write range exceeds the Modbus address space (0~65535)"), 5000);
        logMessage(tr("WRITE rejected out of range: addr=%1 count=%2").arg(start).arg(count), 3);
        return;
    }

    for (uint i = 0; i < writeUnit.valueCount(); i++) {
        const int absolute = start + static_cast<int>(i);
        if (table == QModbusDataUnit::Coils)
            writeUnit.setValue(i, writeModel->m_coils[absolute]);
        else
            writeUnit.setValue(i, writeModel->m_holdingRegisters[absolute]);
    }

    // 分段写入
    auto makeWriteSegments = [](const QModbusDataUnit &unit){
        QList<QModbusDataUnit> segs; int limit = 0;
        switch (unit.registerType()) {
        case QModbusDataUnit::Coils:
        case QModbusDataUnit::DiscreteInputs: limit = 2000; break; // bits (写离散输入不常见，这里同处理)
        case QModbusDataUnit::InputRegisters:
        case QModbusDataUnit::HoldingRegisters:
        default: limit = 123; break; // 多数设备单次写保持寄存器 <= 123
        }
        int remaining = unit.valueCount(), addr = unit.startAddress(), offset = 0;
        while (remaining > 0) {
            const int take = qMin(remaining, limit);
            QModbusDataUnit seg(unit.registerType(), addr, take);
            for (int i = 0; i < take; ++i)
                seg.setValue(i, unit.value(offset + i));
            segs.append(seg);
            addr += take; offset += take; remaining -= take;
        }
        return segs;
    };

    const auto segments = makeWriteSegments(writeUnit);
    for (const auto &seg : segments) {
        logMessage(QStringLiteral("WRITE[%1] addr=%2 count=%3")
                   .arg(seg.registerType()).arg(seg.startAddress()).arg(seg.valueCount()));
        if (auto *reply = modbusDevice->sendWriteRequest(seg, ui->serverEdit->value())) {
            if (!reply->isFinished()) {
                connect(reply, &QModbusReply::finished, this, [this, reply]() {
                    if (reply->error() == QModbusDevice::ProtocolError) {
                        statusBar()->showMessage(tr("Write response error: %1 (Mobus exception: 0x%2)")
                            .arg(reply->errorString()).arg(reply->rawResult().exceptionCode(), -1, 16),
                            5000);
                        logMessage(QStringLiteral("WRITE-ERR protocol=%1 code=0x%2")
                                   .arg(reply->errorString())
                                   .arg(reply->rawResult().exceptionCode(), -1, 16));
                    } else if (reply->error() != QModbusDevice::NoError) {
                        statusBar()->showMessage(tr("Write response error: %1 (code: 0x%2)").
                            arg(reply->errorString()).arg(reply->error(), -1, 16), 5000);
                        logMessage(QStringLiteral("WRITE-ERR %1 code=0x%2")
                                   .arg(reply->errorString()).arg(reply->error(), -1, 16));
                    } else {
                        logMessage(QStringLiteral("WRITE-OK"));
                        // 写入成功后自动回读一次：让「读取结果」立即反映新值，
                        // 无需等待定时轮询 tick 或手动点「读取」
                        on_readButton_clicked();
                    }
                    reply->deleteLater();
                });
            } else {
                // broadcast replies return immediately
                reply->deleteLater();
            }
        } else {
            statusBar()->showMessage(tr("Write error: ") + modbusDevice->errorString(), 5000);
        }
    }
}

void MainWindow::on_readWriteButton_clicked()
{
    if (!modbusDevice)
        return;
    ui->readValue->clear();
    statusBar()->clearMessage();

    QModbusDataUnit writeUnit = writeRequest();
    QModbusDataUnit::RegisterType table = writeUnit.registerType();
    for (uint i = 0; i < writeUnit.valueCount(); i++) {
        if (table == QModbusDataUnit::Coils)
            writeUnit.setValue(i, writeModel->m_coils[i + writeUnit.startAddress()]);
        else
            writeUnit.setValue(i, writeModel->m_holdingRegisters[i + writeUnit.startAddress()]);
    }

    if (auto *reply = modbusDevice->sendReadWriteRequest(readRequest(), writeUnit,
        ui->serverEdit->value())) {
        if (!reply->isFinished())
            connect(reply, &QModbusReply::finished, this, &MainWindow::readReady);
        else
            delete reply; // broadcast replies return immediately
    } else {
        statusBar()->showMessage(tr("Read error: ") + modbusDevice->errorString(), 5000);
    }
}

// ============ 设置持久化/串口枚举/日志实现 ============
void MainWindow::loadSettings()
{
    ui->connectType->setCurrentIndex(m_appSettings.value("ui/connectType", 0).toInt());
    ui->portEdit->setEditText(m_appSettings.value("ui/portEdit", "").toString());
    ui->serverEdit->setValue(m_appSettings.value("ui/serverId", 1).toInt());
    ui->readAddress->setValue(m_appSettings.value("ui/readAddress", 0).toInt());
    ui->writeAddress->setValue(m_appSettings.value("ui/writeAddress", 0).toInt());
    ui->readSize->setEditText(m_appSettings.value("ui/readSize", "10").toString());
    ui->writeSize->setEditText(m_appSettings.value("ui/writeSize", "10").toString());
    m_settingsDialog->loadFromQSettings(m_appSettings);
}

void MainWindow::saveSettings()
{
    m_appSettings.setValue("ui/connectType", ui->connectType->currentIndex());
    m_appSettings.setValue("ui/portEdit", ui->portEdit->currentText());
    m_appSettings.setValue("ui/serverId", ui->serverEdit->value());
    m_appSettings.setValue("ui/readAddress", ui->readAddress->value());
    m_appSettings.setValue("ui/writeAddress", ui->writeAddress->value());
    m_appSettings.setValue("ui/readSize", ui->readSize->currentText());
    m_appSettings.setValue("ui/writeSize", ui->writeSize->currentText());
    m_settingsDialog->saveToQSettings(m_appSettings);
}

void MainWindow::populateSerialPorts()
{
    ui->portEdit->clear();
    QStringList ports;
    const auto list = QSerialPortInfo::availablePorts();
    for (const auto &info : list) {
        QString name = info.portName();
        if (!name.isEmpty()) ports << name;
    }
    if (!ports.isEmpty())
        ui->portEdit->addItems(ports);
}

void MainWindow::logMessage(const QString &line, int level) const
{
    static const char* levelTags[] = {"[DEBUG] ", "[INFO] ", "[WARNING] ", "[ERROR] "};
    const char* tag = (level >= 0 && level < 4) ? levelTags[level] : "[INFO] ";

    const QString dir = QCoreApplication::applicationDirPath() + "/logs";
    QDir logDir(dir);
    // mkpath 只做一次：每次日志都做一次文件系统查询，
    // 在定时读取（100ms 一次）时是每秒 10+ 次 CreateFile/FindFirstFile，GUI 线程抖动明显。
    static bool logDirEnsured = false;
    if (!logDirEnsured) {
        logDir.mkpath(".");
        logDirEnsured = true;
    }

    static QDate lastCleanupDate;
    const QDate today = QDate::currentDate();
    if (lastCleanupDate != today) {
        lastCleanupDate = today;
        const QDate expireDate = today.addDays(-30);
        const QFileInfoList oldLogs = logDir.entryInfoList(QStringList() << "*.log", QDir::Files);
        for (const QFileInfo &info : oldLogs) {
            const QDate fileDate = QDate::fromString(info.completeBaseName(), "yyyyMMdd");
            if (fileDate.isValid() && fileDate < expireDate)
                QFile::remove(info.absoluteFilePath());
        }
    }

    const QString filePath = logDir.absoluteFilePath(QDate::currentDate().toString("yyyyMMdd") + ".log");
    QFile f(filePath);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream s(&f);
        s.setGenerateByteOrderMark(false);
        s << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ") << tag << line << "\n";
        s.flush();
        f.close();
    }
}

void MainWindow::on_writeTable_currentIndexChanged(int index)
{
    const bool coilsOrHolding = index == 0 || index == 3;
    if (coilsOrHolding) {
        ui->writeValueTable->setColumnHidden(1, index != 0);
        ui->writeValueTable->setColumnHidden(2, index != 3);
        ui->writeValueTable->resizeColumnToContents(0);
    }

    // 写入区小字提示：随寄存器类型切换，说明怎么填值
    if (index == 3)
        ui->writeHintLabel->setText(tr("Holding register: double-click a cell to enter a hex value, then press Write."));
    else if (index == 0)
        ui->writeHintLabel->setText(tr("Coil: check = write 1 (ON), uncheck = write 0 (OFF), then press Write."));
    else
        ui->writeHintLabel->setText(tr("This register type is read-only and cannot be written."));

    ui->readWriteButton->setEnabled(index == 3);
    ui->writeButton->setEnabled(coilsOrHolding);
    ui->writeGroupBox->setEnabled(coilsOrHolding);
}

QModbusDataUnit MainWindow::readRequest() const
{
    const auto table =
        static_cast<QModbusDataUnit::RegisterType> (ui->writeTable->currentData().toInt());

    int startAddress = ui->readAddress->value();
    bool ok = false;
    int numberOfEntries = ui->readSize->currentText().toInt(&ok);
    if (!ok) numberOfEntries = 0;
    if (startAddress < 0) startAddress = 0;
    // 上限保护：readSize 是可编辑下拉框，用户可输入任意数量。
    // 不加限制时输入 100000 会切成 800 段一次性向设备发出 800 个读请求，
    // 把现场从站打爆（请求排队堆积、超时、丢包）。
    const int limit = (table == QModbusDataUnit::Coils || table == QModbusDataUnit::DiscreteInputs)
                          ? 2000 : 125;
    if (numberOfEntries < 0) numberOfEntries = 0;
    if (numberOfEntries > limit) numberOfEntries = limit;
    return QModbusDataUnit(table, startAddress, numberOfEntries);
}

QModbusDataUnit MainWindow::writeRequest() const
{
    const auto table =
        static_cast<QModbusDataUnit::RegisterType> (ui->writeTable->currentData().toInt());

    int startAddress = ui->writeAddress->value();
    bool ok = false;
    int numberOfEntries = ui->writeSize->currentText().toInt(&ok);
    if (!ok) numberOfEntries = 0;
    if (startAddress < 0) startAddress = 0;
    // 单次写入上限（Modbus 规范：保持寄存器 ≤123，位区 ≤2000）
    const int limit = (table == QModbusDataUnit::Coils || table == QModbusDataUnit::DiscreteInputs)
                          ? 2000 : 123;
    if (numberOfEntries < 0) numberOfEntries = 0;
    if (numberOfEntries > limit) numberOfEntries = limit;
    return QModbusDataUnit(table, startAddress, numberOfEntries);
}

void MainWindow::initAdvancedFeatures()
{
    m_logViewer = new LogViewer(this);
    m_logViewer->setLogDirectory(QCoreApplication::applicationDirPath() + "/logs");
    m_pollManager = new PollManager(this);
    connect(m_pollManager, &PollManager::pollRequest, this, &MainWindow::onPollRequest);
    m_dataExporter = new DataExporter(this);
    m_dataExporter->setMaxRecords(10000);
    m_chart = new RealTimeChart();
    m_chart->setWindowTitle(tr("Real-time Data Chart"));
    m_chart->resize(600, 400);
    m_deviceManager = new DeviceManager(this);
    m_templateManager = new DeviceTemplateManager(this);
    // 必须在这里加载模板：析构时会无条件把模板写回 templates.json。
    // 若启动时不加载，内存里模板为空 → 重启后再退出就把磁盘上的模板覆盖成 []，
    // 用户资产被应用自身清空且无任何提示。
    m_templateManager->loadFromFile(QCoreApplication::applicationDirPath() + "/templates.json");
    m_scriptEngine = new ScriptEngine(this);
    m_remoteServer = new RemoteServer(this);
    m_remoteServer->setStatusProvider([this]() { return buildRemoteStatus(); });
    m_remoteServer->setReadHandler([this](int serverAddress, int registerType, int startAddress, int count,
                                           const RemoteServer::ResponseCallback &callback) {
        executeRemoteRead(serverAddress, registerType, startAddress, count, callback);
    });
    m_remoteServer->setWriteHandler([this](int serverAddress, int registerType, int startAddress,
                                            const QVector<quint16> &values,
                                            const RemoteServer::ResponseCallback &callback) {
        executeRemoteWrite(serverAddress, registerType, startAddress, values, callback);
    });
    m_historyData = new HistoryData(this);
    // 历史库必须放在可写目录：装到 C:\Program Files 下程序目录不可写，
    // openDatabase 会失败 → 历史归档静默失效且界面无任何提示。
    const QString historyPath = m_appSettings.value(
        "history/dbPath",
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/history.db").toString();
    if (!m_historyData->openDatabase(historyPath)) {
        const QString err = m_historyData->lastError();
        logMessage(tr("Failed to open history database: %1 (history archiving unavailable)").arg(err), 3);
        statusBar()->showMessage(tr("Cannot open history database: %1").arg(err), 8000);
    }
    m_pluginManager = new PluginManager(this);
    m_pluginManager->setPluginDirectory(QCoreApplication::applicationDirPath() + "/plugins");
    m_alarmManager = new AlarmManager(this);

    // 报警规则/事件/活跃状态持久化：独立 alarms.db，避免与高频历史写入争锁。
    const QString alarmPath = m_appSettings.value(
        QStringLiteral("alarm/dbPath"),
        QFileInfo(historyPath).absolutePath() + QStringLiteral("/alarms.db")).toString();
    m_alarmRepository.reset(new AlarmRepository());
    if (m_alarmRepository->open(alarmPath)) {
        connect(m_alarmManager, &AlarmManager::storageError, this, [this](const QString &message) {
            logMessage(tr("Alarm persistence failed: %1 (in-memory alarms continue)").arg(message), 3);
        });
        m_alarmManager->setRepository(m_alarmRepository.data());
        logMessage(tr("Alarm persistence enabled: %1 (%2 rules, %3 events)")
                       .arg(alarmPath)
                       .arg(m_alarmRepository->ruleCount())
                       .arg(m_alarmRepository->eventCount()));
    } else {
        const QString error = m_alarmRepository->lastError();
        m_alarmRepository.reset();
        logMessage(tr("Failed to open alarm database: %1 (rules/events are in memory only and will be lost on restart)")
                       .arg(error), 3);
    }

    m_batchTaskManager = new BatchTaskManager(this);
    connect(m_batchTaskManager, &BatchTaskManager::executeReadTask, this, &MainWindow::onBatchReadTask);
    connect(m_batchTaskManager, &BatchTaskManager::executeWriteTask, this, &MainWindow::onBatchWriteTask);
    m_dashboard = new Dashboard();
    m_dashboard->setWindowTitle(tr("Industrial Data Dashboard"));
    m_dashboard->resize(900, 650);

    GaugeConfig valueGauge;
    valueGauge.name = tr("Current Value");
    valueGauge.unit = "raw";
    valueGauge.minValue = 0;
    valueGauge.maxValue = 65535;
    valueGauge.warningThreshold = 50000;
    valueGauge.criticalThreshold = 60000;
    valueGauge.currentValue = 0;
    valueGauge.normalColor = QColor(46, 204, 113);
    valueGauge.warningColor = QColor(241, 196, 15);
    valueGauge.criticalColor = QColor(231, 76, 60);
    m_dashboard->addGauge(valueGauge);

    GaugeConfig addressGauge = valueGauge;
    addressGauge.name = tr("Start Address");
    addressGauge.unit = "addr";
    addressGauge.maxValue = 65535;
    addressGauge.warningThreshold = 50000;
    addressGauge.criticalThreshold = 60000;
    addressGauge.currentValue = ui->readAddress->value();
    m_dashboard->addGauge(addressGauge);

    GaugeConfig countGauge = valueGauge;
    countGauge.name = tr("Read Count");
    countGauge.unit = "count";
    countGauge.maxValue = 2000;
    countGauge.warningThreshold = 1000;
    countGauge.criticalThreshold = 1800;
    countGauge.currentValue = ui->readSize->currentText().toInt();
    m_dashboard->addGauge(countGauge);

    GaugeConfig alarmGauge = valueGauge;
    alarmGauge.name = tr("Active Alarms");
    alarmGauge.unit = tr("count");
    alarmGauge.maxValue = 100;
    alarmGauge.warningThreshold = 1;
    alarmGauge.criticalThreshold = 5;
    alarmGauge.currentValue = 0;
    m_dashboard->addGauge(alarmGauge);
    m_dashboard->setConnectionStatus(false, tr("Waiting for connection"));
    m_dashboard->setPollingStatus(false);
    m_dashboard->setLastUpdateTime(QDateTime());
    m_dashboard->setServerAddress(ui->serverEdit->value());
    m_dashboard->setAlarmCount(0);
    m_dashboard->appendCommunicationLog(tr("Dashboard initialized"));
    m_configProfileManager = new ConfigProfileManager(this);
    m_configProfileManager->setProfileDirectory(QCoreApplication::applicationDirPath() + "/profiles");

    m_reliabilityManager = new ReliabilityManager(this);
    m_securityManager = new SecurityManager(this);
    m_securityManager->load(m_appSettings);
    m_remoteServer->setSecurityManager(m_securityManager);
    m_remoteServer->setRemoteWriteEnabled(m_securityManager->remoteWriteEnabled());
    m_pointModel = new PointModel(this);
    const QString pointFile = QCoreApplication::applicationDirPath() + "/points.json";
    if (!m_pointModel->loadFromFile(pointFile)) {
        PointDefinition point;
        point.name = tr("Default holding register 0");
        point.serverAddress = ui->serverEdit->value();
        point.registerType = QModbusDataUnit::HoldingRegisters;
        point.address = 0;
        point.count = 1;
        point.dataType = QStringLiteral("uint16");
        point.unit = QStringLiteral("PV");
        m_pointModel->addPoint(point);
        m_pointModel->saveToFile(pointFile);
    }
    connect(m_pointModel, &PointModel::pointValueUpdated, this, [this](const PointDefinition &point, const PointValue &value) {
        // 析构期间 m_chart / m_dashboard 可能已被删除 → 必须判空
        if (m_shuttingDown)
            return;
        if (m_chart && m_pointChartSeriesMap.contains(point.id))
            m_chart->addDataPoint(m_pointChartSeriesMap.value(point.id), value.value);
        if (m_dashboard && m_pointDashboardGaugeMap.contains(point.id))
            m_dashboard->updateGaugeValue(m_pointDashboardGaugeMap.value(point.id), value.value);

        // 工程值报警：AlarmManager 里的点位规则按 scale/offset 换算后的值判定，
        // 不再把原始寄存器值当成工程量（scale≠1 时旧实现必然误报/漏报）。
        if (m_alarmManager)
            m_alarmManager->checkPointValue(point.deviceId, point.id, value.value,
                                            static_cast<int>(value.quality));

        // 归档开关 / 归档周期 / 死区在这里真正生效
        maybeArchivePoint(point, value);
    });

    // 陈旧看门狗：轮询运行中，点位超过 3×采集周期（且 ≥5s）未更新 → Timeout
    m_qualityWatchdog = new QTimer(this);
    m_qualityWatchdog->setInterval(1000);
    connect(m_qualityWatchdog, &QTimer::timeout, this, &MainWindow::updateQualityWatchdog);
    m_qualityWatchdog->start();

    // 设备断开 → 该设备下所有点位质量码降级（否则界面永远显示最后一次 Good 值）
    connect(m_deviceManager, &DeviceManager::deviceDisconnected, this, [this](int deviceId) {
        if (m_pointModel)
            m_pointModel->markDeviceBad(deviceId, DataQuality::Bad,
                                        QStringLiteral("device %1 disconnected").arg(deviceId));
        logMessage(tr("Device %1 disconnected; related point quality set to Bad").arg(deviceId), 2);
    });
    connect(m_deviceManager, &DeviceManager::deviceError, this, [this](int deviceId, const QString &error) {
        logMessage(tr("Device %1 communication error: %2").arg(deviceId).arg(error), 3);
    });
    m_verificationManager = new VerificationManager(this);
    m_deliveryManager = new DeliveryManager(this);

    connect(m_reliabilityManager, &ReliabilityManager::reconnectRequested, this, [this]() {
        if (modbusDevice && modbusDevice->state() == QModbusDevice::UnconnectedState) {
            logMessage(tr("Unexpected disconnect, reconnecting automatically..."), 2);
            // 直接执行连接动作（非手动：失败不弹窗）；用户意图已在重连门控中校验
            attemptModbusConnection(false);
        }
    });
    connect(m_reliabilityManager, &ReliabilityManager::heartbeatRequested, this, [this]() {
        if (modbusDevice && modbusDevice->state() == QModbusDevice::ConnectedState) {
            PollTask heartbeat;
            heartbeat.id = 0;
            heartbeat.name = QStringLiteral("Heartbeat");
            heartbeat.deviceId = 0;
            heartbeat.serverAddress = ui->serverEdit->value();
            heartbeat.registerType = QModbusDataUnit::HoldingRegisters;
            heartbeat.startAddress = ui->readAddress->value();
            heartbeat.quantity = 1;
            heartbeat.intervalMs = 5000;
            heartbeat.enabled = true;
            onPollRequest(heartbeat);
        }
    });
    connect(m_reliabilityManager, &ReliabilityManager::continuousFailureAlarm, this, [this](int count, const QString &reason) {
        m_dashboard->appendAlarmLog(tr("Consecutive communication failures: %1 (%2)").arg(count).arg(reason));
        logMessage(tr("Consecutive communication failures: %1 (%2)").arg(count).arg(reason), 3);
    });
    m_reliabilityManager->start();

    m_deviceManager->loadFromSettings(m_appSettings);

    // ---------- MQTT：初始化发布端客户端与遥测/报警挂钩 ----------
    initMqttSupport();
}

void MainWindow::initMenus()
{
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(tr("Log Viewer"), this, &MainWindow::showLogViewer);
    viewMenu->addAction(tr("Real-time Chart"), this, &MainWindow::showChart);
    viewMenu->addAction(tr("Data Dashboard"), this, &MainWindow::showDashboard);

    // ---------- 界面主题切换（深色工业风 / 浅色现代风） ----------
    QMenu *themeMenu = viewMenu->addMenu(tr("Interface Theme"));
    QActionGroup *themeGroup = new QActionGroup(themeMenu);
    themeGroup->setExclusive(true);
    const QString currentTheme = m_appSettings.value(
        QStringLiteral("ui/theme"), QStringLiteral("dark")).toString();
    const QStringList themes = ThemeManager::availableThemes();
    for (const QString &themeId : themes) {
        QAction *themeAction = themeMenu->addAction(
            themeId == QLatin1String("light") ? tr("Light Modern") : tr("Dark Industrial"));
        themeAction->setCheckable(true);
        themeAction->setChecked(themeId == currentTheme);
        themeGroup->addAction(themeAction);
        connect(themeAction, &QAction::triggered, this, [this, themeId]() {
            ThemeManager::applyTheme(themeId);       // 立即生效
            m_appSettings.setValue("ui/theme", themeId);  // 并持久化记忆
        });
    }

    // ---------- 界面语言切换（简体中文 / English） ----------
    // .ui 内的控件文字立即重译；代码里创建的菜单标题在重启后完全生效。
    QMenu *langMenu = viewMenu->addMenu(tr("Language"));
    QActionGroup *langGroup = new QActionGroup(langMenu);
    langGroup->setExclusive(true);
    const QString currentLang = m_appSettings.value(
        QStringLiteral("ui/language"), QLocale::system().name()).toString();
    const bool english = currentLang.startsWith(QLatin1String("en"));
    QAction *zhAction = langMenu->addAction(tr("Simplified Chinese"));
    QAction *enAction = langMenu->addAction(QStringLiteral("English"));
    zhAction->setCheckable(true);
    enAction->setCheckable(true);
    zhAction->setChecked(!english);
    enAction->setChecked(english);
    langGroup->addAction(zhAction);
    langGroup->addAction(enAction);
    connect(zhAction, &QAction::triggered, this, [this]() {
        applyLanguage(QStringLiteral("zh_CN"));
    });
    connect(enAction, &QAction::triggered, this, [this]() {
        applyLanguage(QStringLiteral("en"));
    });

    QMenu *dataMenu = menuBar()->addMenu(tr("&Data"));
    dataMenu->addAction(tr("Export CSV"), this, &MainWindow::exportData);
    dataMenu->addAction(tr("Scheduled Polling"), this, &MainWindow::startStopPolling);
    dataMenu->addAction(tr("Batch Tasks"), this, &MainWindow::showBatchTaskDialog);
    dataMenu->addAction(tr("Alarm Config"), this, &MainWindow::showAlarmConfig);
    dataMenu->addAction(tr("History Query"), this, &MainWindow::showHistoryQuery);
    dataMenu->addAction(tr("Import Points..."), this, &MainWindow::importPointsFile);
    dataMenu->addAction(tr("Export Points..."), this, &MainWindow::exportPointsFile);
    QMenu *advMenu = menuBar()->addMenu(tr("&Advanced"));
    advMenu->addAction(tr("Device Manager"), this, &MainWindow::showDeviceManager);
    advMenu->addAction(tr("Bus Scanner"), this, &MainWindow::showScanner);
    advMenu->addAction(tr("Raw Request (Tools)"), this, &MainWindow::showRawRequestTool);
    advMenu->addAction(tr("Device Templates"), this, &MainWindow::showTemplateManager);
    advMenu->addAction(tr("Script Console"), this, &MainWindow::showScriptConsole);
    advMenu->addAction(tr("Remote Service"), this, &MainWindow::toggleRemoteServer);
    advMenu->addAction(tr("MQTT Publishing"), this, &MainWindow::showMqttSettings);
    advMenu->addAction(tr("Database Settings"), this, &MainWindow::showDatabaseSettings);
    advMenu->addAction(tr("Simulator (Modbus Slave)"), this, &MainWindow::showDeviceSimulator);
    advMenu->addAction(tr("Plugin Manager"), this, &MainWindow::showPluginManager);
    advMenu->addAction(tr("Point Manager"), this, &MainWindow::showPointManager);
    advMenu->addAction(tr("Verification Tests"), this, &MainWindow::showVerificationManager);
    advMenu->addAction(tr("Product Delivery"), this, &MainWindow::showDeliveryManager);
    QMenu *profileMenu = menuBar()->addMenu(tr("&Profile"));
    profileMenu->addAction(tr("Save Profile"), this, &MainWindow::saveProfile);
    profileMenu->addAction(tr("Load Profile"), this, &MainWindow::loadProfile);
}

// ============ 界面状态胶囊与多语言 ============

// 顶部品牌栏右侧的连接状态胶囊：未连接红色 / 已连接绿色（样式见 style/*.qss）
void MainWindow::updateConnectionChip(bool connected)
{
    if (!ui->statusChip)
        return;

    const QString endpoint = (ui->portEdit && !ui->portEdit->currentText().isEmpty())
        ? ui->portEdit->currentText()
        : QStringLiteral("--");

    ui->statusChip->setProperty("connected", connected);
    ui->statusChip->setText(connected
        ? tr("● Connected · %1").arg(endpoint)
        : tr("● Offline"));
    // 动态属性变化后需要重新 polish 才能让 QSS 选择器生效
    ui->statusChip->style()->unpolish(ui->statusChip);
    ui->statusChip->style()->polish(ui->statusChip);
}

// 应用界面语言：zh 系加载嵌入翻译，en 系使用源文（英文）
void MainWindow::applyLanguage(const QString &lang)
{
    // 归一化：en_US / en_GB 等 en 前缀一律按英文处理
    const bool english = lang.startsWith(QLatin1String("en"));
    m_appSettings.setValue(QStringLiteral("ui/language"),
        english ? QStringLiteral("en") : QStringLiteral("zh_CN"));

    if (m_translator) {
        qApp->removeTranslator(m_translator);
        if (!english) {
            if (m_translator->load(QStringLiteral("fieldlink_zh_CN"), QStringLiteral(":/i18n")))
                qApp->installTranslator(m_translator);
        }
    }

    // 重译 .ui 内所有控件（菜单栏 Device/Tools、按钮、标签等）
    if (ui->centralWidget)
        ui->retranslateUi(this);

    // 刷新运行时动态文本
    const bool connected = modbusDevice
        && modbusDevice->state() != QModbusDevice::UnconnectedState;
    ui->connectButton->setText(connected ? tr("Disconnect") : tr("Connect"));
    updateConnectionChip(connected);
    statusBar()->showMessage(tr("Ready | FieldLink industrial Modbus TCP/RTU communication and data acquisition platform"));
}
