
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
#include "batchtaskmanager.h"
#include "dashboard.h"
#include "configprofile.h"
#include "reliabilitymanager.h"
#include "securitymanager.h"
#include "pointmodel.h"
#include "verificationmanager.h"
#include "deliverymanager.h"
#include "thememanager.h"  // 深色/浅色主题切换
#include "mcpserver.h"     // AI/MCP：MCP 服务器
#include "agenttool.h"     // AI/MCP：共用工具注册表
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
#include <QStatusBar>
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

    delete m_chart;
    m_chart = nullptr;
    delete m_dashboard;
    m_dashboard = nullptr;

    if (modbusDevice)
        modbusDevice->disconnectDevice();
    delete modbusDevice;

    saveSettings();

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
        // 用户明确要求连接：置位意图，意外断线时才会自动重连
        m_reliabilityManager->setUserIntentConnected(true);
        attemptModbusConnection();
    } else {
        // 用户主动断开：清除意图，自动重连循环立即停止
        m_reliabilityManager->setUserIntentConnected(false);
        modbusDevice->disconnectDevice();
        ui->actionConnect->setEnabled(true);
        ui->actionDisconnect->setEnabled(false);
        logMessage(QStringLiteral("已断开连接"));
    }
}

// 执行一次连接动作（参数装配 + connectDevice）。由手动连接与自动重连共同复用，
// 不包含用户意图切换，保证自动重连不会误改意图状态。
void MainWindow::attemptModbusConnection()
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
        statusBar()->showMessage(tr("Connect failed: ") + modbusDevice->errorString(), 5000);
    } else {
        ui->actionConnect->setEnabled(false);
        ui->actionDisconnect->setEnabled(true);
        // 记录连接成功日志
        logMessage(QStringLiteral("连接成功: %1").arg(ui->portEdit->currentText()));
    }
}

void MainWindow::onStateChanged(int state)
{
    bool connected = (state != QModbusDevice::UnconnectedState);
    ui->actionConnect->setEnabled(!connected);
    ui->actionDisconnect->setEnabled(connected);

    if (state == QModbusDevice::UnconnectedState)
        ui->connectButton->setText(tr("Connect"));
    else if (state == QModbusDevice::ConnectedState)
        ui->connectButton->setText(tr("Disconnect"));

    updateConnectionChip(connected);

    // MQTT：设备连接状态变化时发布保留消息（上位机/网关可据此判断现场在线）
    publishMqttStatus(connected);

    if (m_dashboard) {
        m_dashboard->setConnectionStatus(connected,
            QStringLiteral("%1 %2").arg(connected ? "已连接" : "已断开", ui->portEdit->currentText()));
        m_dashboard->setServerAddress(ui->serverEdit->value());
    }
    if (m_reliabilityManager) {
        if (connected)
            m_reliabilityManager->notifySuccess();
        else
            m_reliabilityManager->notifyDisconnected();
    }
    saveSettings();
}

void MainWindow::on_readButton_clicked()
{
    if (!modbusDevice)
        return;
    ui->readValue->clear();
    statusBar()->clearMessage();

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
    for (const auto &seg : segments) {
        logMessage(QStringLiteral("READ[%1] addr=%2 count=%3")
                   .arg(seg.registerType()).arg(seg.startAddress()).arg(seg.valueCount()));
        if (auto *reply = modbusDevice->sendReadRequest(seg, ui->serverEdit->value())) {
            if (!reply->isFinished())
                connect(reply, &QModbusReply::finished, this, &MainWindow::readReady);
            else
                delete reply; // 广播回复会立即返回
        } else {
            statusBar()->showMessage(tr("Read error: ") + modbusDevice->errorString(), 5000);
        }
    }
}

void MainWindow::readReady()
{
    auto reply = qobject_cast<QModbusReply *>(sender());
    if (!reply)
        return;

    if (reply->error() == QModbusDevice::NoError) {
        const QModbusDataUnit unit = reply->result();
        ui->readValue->setUpdatesEnabled(false);
        QListWidgetItem *lastItem = nullptr;
        for (uint i = 0; i < unit.valueCount(); i++) {
            const QString entry = tr("Address: %1, Value: %2").arg(unit.startAddress() + i)
                                     .arg(QString::number(unit.value(i),
                                          unit.registerType() <= QModbusDataUnit::Coils ? 10 : 16));
            lastItem = new QListWidgetItem(entry);
            ui->readValue->addItem(lastItem);
        }
        ui->readValue->setUpdatesEnabled(true);
        if (lastItem)
            ui->readValue->scrollToItem(lastItem, QAbstractItemView::PositionAtBottom);
        logMessage(QStringLiteral("READ-OK addr=%1 count=%2").arg(unit.startAddress()).arg(unit.valueCount()));
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

    reply->deleteLater();
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
    for (uint i = 0; i < writeUnit.valueCount(); i++) {
        if (table == QModbusDataUnit::Coils)
            writeUnit.setValue(i, writeModel->m_coils[i + writeUnit.startAddress()]);
        else
            writeUnit.setValue(i, writeModel->m_holdingRegisters[i + writeUnit.startAddress()]);
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
    logDir.mkpath(".");

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

    ui->readWriteButton->setEnabled(index == 3);
    ui->writeButton->setEnabled(coilsOrHolding);
    ui->writeGroupBox->setEnabled(coilsOrHolding);
}

QModbusDataUnit MainWindow::readRequest() const
{
    const auto table =
        static_cast<QModbusDataUnit::RegisterType> (ui->writeTable->currentData().toInt());

    int startAddress = ui->readAddress->value();
    int numberOfEntries = ui->readSize->currentText().toInt();
    if (startAddress < 0) startAddress = 0;
    if (numberOfEntries < 0) numberOfEntries = 0;
    return QModbusDataUnit(table, startAddress, numberOfEntries);
}

QModbusDataUnit MainWindow::writeRequest() const
{
    const auto table =
        static_cast<QModbusDataUnit::RegisterType> (ui->writeTable->currentData().toInt());

    int startAddress = ui->writeAddress->value();
    int numberOfEntries = ui->writeSize->currentText().toInt();
    if (startAddress < 0) startAddress = 0;
    if (numberOfEntries < 0) numberOfEntries = 0;
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
    m_chart->setWindowTitle("实时数据曲线");
    m_chart->resize(600, 400);
    m_deviceManager = new DeviceManager(this);
    m_templateManager = new DeviceTemplateManager(this);
    m_scriptEngine = new ScriptEngine(this);
    m_remoteServer = new RemoteServer(this);
    m_remoteServer->setStatusProvider([this]() { return buildRemoteStatus(); });
    m_remoteServer->setReadHandler([this](int serverAddress, int registerType, int startAddress, int count) {
        return executeRemoteRead(serverAddress, registerType, startAddress, count);
    });
    m_remoteServer->setWriteHandler([this](int serverAddress, int registerType, int startAddress, const QVector<quint16> &values) {
        return executeRemoteWrite(serverAddress, registerType, startAddress, values);
    });
    m_historyData = new HistoryData(this);
    m_historyData->openDatabase(QCoreApplication::applicationDirPath() + "/history.db");
    m_pluginManager = new PluginManager(this);
    m_pluginManager->setPluginDirectory(QCoreApplication::applicationDirPath() + "/plugins");
    m_alarmManager = new AlarmManager(this);
    m_batchTaskManager = new BatchTaskManager(this);
    connect(m_batchTaskManager, &BatchTaskManager::executeReadTask, this, &MainWindow::onBatchReadTask);
    connect(m_batchTaskManager, &BatchTaskManager::executeWriteTask, this, &MainWindow::onBatchWriteTask);
    m_dashboard = new Dashboard();
    m_dashboard->setWindowTitle("工业数据监控大屏");
    m_dashboard->resize(900, 650);

    GaugeConfig valueGauge;
    valueGauge.name = "当前值";
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
    addressGauge.name = "起始地址";
    addressGauge.unit = "addr";
    addressGauge.maxValue = 65535;
    addressGauge.warningThreshold = 50000;
    addressGauge.criticalThreshold = 60000;
    addressGauge.currentValue = ui->readAddress->value();
    m_dashboard->addGauge(addressGauge);

    GaugeConfig countGauge = valueGauge;
    countGauge.name = "读取数量";
    countGauge.unit = "count";
    countGauge.maxValue = 2000;
    countGauge.warningThreshold = 1000;
    countGauge.criticalThreshold = 1800;
    countGauge.currentValue = ui->readSize->currentText().toInt();
    m_dashboard->addGauge(countGauge);

    GaugeConfig alarmGauge = valueGauge;
    alarmGauge.name = "活跃报警";
    alarmGauge.unit = "个";
    alarmGauge.maxValue = 100;
    alarmGauge.warningThreshold = 1;
    alarmGauge.criticalThreshold = 5;
    alarmGauge.currentValue = 0;
    m_dashboard->addGauge(alarmGauge);
    m_dashboard->setConnectionStatus(false, QStringLiteral("等待连接"));
    m_dashboard->setPollingStatus(false);
    m_dashboard->setLastUpdateTime(QDateTime());
    m_dashboard->setServerAddress(ui->serverEdit->value());
    m_dashboard->setAlarmCount(0);
    m_dashboard->appendCommunicationLog(QStringLiteral("监控大屏初始化完成"));
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
        point.name = QStringLiteral("默认保持寄存器0");
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
        if (m_pointChartSeriesMap.contains(point.id))
            m_chart->addDataPoint(m_pointChartSeriesMap.value(point.id), value.value);
        if (m_pointDashboardGaugeMap.contains(point.id))
            m_dashboard->updateGaugeValue(m_pointDashboardGaugeMap.value(point.id), value.value);
    });
    m_verificationManager = new VerificationManager(this);
    m_deliveryManager = new DeliveryManager(this);

    connect(m_reliabilityManager, &ReliabilityManager::reconnectRequested, this, [this]() {
        if (modbusDevice && modbusDevice->state() == QModbusDevice::UnconnectedState) {
            logMessage(QStringLiteral("意外断线，正在自动重连..."), 2);
            // 直接执行连接动作；用户意图已在自动重连门控中校验
            attemptModbusConnection();
        }
    });
    connect(m_reliabilityManager, &ReliabilityManager::heartbeatRequested, this, [this]() {
        if (modbusDevice && modbusDevice->state() == QModbusDevice::ConnectedState)
            onPollRequest(PollTask{0, QStringLiteral("Heartbeat"), ui->serverEdit->value(), QModbusDataUnit::HoldingRegisters, ui->readAddress->value(), 1, 5000, true, false, 0, 0});
    });
    connect(m_reliabilityManager, &ReliabilityManager::continuousFailureAlarm, this, [this](int count, const QString &reason) {
        m_dashboard->appendAlarmLog(QStringLiteral("连续通信失败 %1 次: %2").arg(count).arg(reason));
        logMessage(QStringLiteral("连续通信失败 %1 次: %2").arg(count).arg(reason), 3);
    });
    m_reliabilityManager->start();

    m_deviceManager->loadFromSettings(m_appSettings);

    // ---------- AI/MCP：初始化共用工具注册表与 MCP 服务器 ----------
    initMcpAgent();

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
    QAction *zhAction = langMenu->addAction(QStringLiteral("简体中文"));
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
    QMenu *advMenu = menuBar()->addMenu(tr("&Advanced"));
    advMenu->addAction(tr("Device Manager"), this, &MainWindow::showDeviceManager);
    advMenu->addAction(tr("Device Templates"), this, &MainWindow::showTemplateManager);
    advMenu->addAction(tr("Script Console"), this, &MainWindow::showScriptConsole);
    advMenu->addAction(tr("Remote Service"), this, &MainWindow::toggleRemoteServer);
    advMenu->addAction(tr("MCP Service (AI)"), this, &MainWindow::toggleMcpServer);
    advMenu->addAction(tr("MQTT Publishing"), this, &MainWindow::showMqttSettings);
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
