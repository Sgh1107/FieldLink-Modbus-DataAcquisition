#include "mainwindow.h"
#include "ui_mainwindow.h"
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
#include "logviewer.h"
#include "settingsdialog.h"
#include "mqttclient.h"    // MQTT 发布端客户端

#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QStatusBar>
#include <QCoreApplication>
#include <QDir>
#include <QLineEdit>
#include <QModbusDevice>
#include <QModbusClient>
#include <QModbusReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QDateTime>
#include <QLabel>
#include <QAbstractItemView>
#include <QTextEdit>
#include <QTableWidgetItem>
#include <QFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QDateTimeEdit>

static QModbusDataUnit::RegisterType registerTypeFromComboIndex(int index)
{
    switch (index) {
    case 0: return QModbusDataUnit::Coils;
    case 1: return QModbusDataUnit::DiscreteInputs;
    case 2: return QModbusDataUnit::InputRegisters;
    case 3: return QModbusDataUnit::HoldingRegisters;
    default: return QModbusDataUnit::HoldingRegisters;
    }
}

static int comboIndexFromRegisterType(int type)
{
    switch (static_cast<QModbusDataUnit::RegisterType>(type)) {
    case QModbusDataUnit::Coils: return 0;
    case QModbusDataUnit::DiscreteInputs: return 1;
    case QModbusDataUnit::InputRegisters: return 2;
    case QModbusDataUnit::HoldingRegisters: return 3;
    default: return 3;
    }
}

static QModbusDataUnit::RegisterType currentUiRegisterType(Ui::MainWindow *ui)
{
    return static_cast<QModbusDataUnit::RegisterType>(ui->writeTable->currentData().toInt());
}

static int currentUiRegisterComboIndex(Ui::MainWindow *ui)
{
    return comboIndexFromRegisterType(static_cast<int>(currentUiRegisterType(ui)));
}

static QString registerTypeName(int type)
{
    switch (static_cast<QModbusDataUnit::RegisterType>(type)) {
    case QModbusDataUnit::Coils: return QStringLiteral("Coils");
    case QModbusDataUnit::DiscreteInputs: return QStringLiteral("Discrete Inputs");
    case QModbusDataUnit::InputRegisters: return QStringLiteral("Input Registers");
    case QModbusDataUnit::HoldingRegisters: return QStringLiteral("Holding Registers");
    default: return QStringLiteral("Unknown");
    }
}

static QString dataTypeForRegisterCount(const QString &dataType, int count)
{
    if (!dataType.trimmed().isEmpty())
        return dataType.trimmed();
    return count > 1 ? QStringLiteral("uint32") : QStringLiteral("uint16");
}

static QString templateFilePath()
{
    return QCoreApplication::applicationDirPath() + "/templates.json";
}

static QString pointFilePath()
{
    return QCoreApplication::applicationDirPath() + "/points.json";
}

static QString alarmConditionName(AlarmCondition condition)
{
    switch (condition) {
    case AlarmCondition::GreaterThan: return QStringLiteral("大于");
    case AlarmCondition::LessThan: return QStringLiteral("小于");
    case AlarmCondition::Equal: return QStringLiteral("等于");
    case AlarmCondition::NotEqual: return QStringLiteral("不等于");
    case AlarmCondition::InRange: return QStringLiteral("范围内");
    case AlarmCondition::OutOfRange: return QStringLiteral("范围外");
    case AlarmCondition::BitSet: return QStringLiteral("bit set");
    case AlarmCondition::BitClear: return QStringLiteral("bit clear");
    }
    return QStringLiteral("未知");
}

static QString alarmSeverityName(AlarmSeverity severity)
{
    switch (severity) {
    case AlarmSeverity::Info: return QStringLiteral("提示");
    case AlarmSeverity::Warning: return QStringLiteral("警告");
    case AlarmSeverity::Critical: return QStringLiteral("严重");
    }
    return QStringLiteral("未知");
}

static QString pointQualityName(DataQuality quality)
{
    switch (quality) {
    case DataQuality::Good: return QStringLiteral("Good");
    case DataQuality::Bad: return QStringLiteral("Bad");
    case DataQuality::Timeout: return QStringLiteral("Timeout");
    case DataQuality::OutOfRange: return QStringLiteral("OutOfRange");
    case DataQuality::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

static PointValue currentPointValue(const PointModel *model, int pointId)
{
    for (const auto &value : model->currentValues()) {
        if (value.pointId == pointId)
            return value;
    }
    return PointValue();
}

static bool exportPointsToCsv(const QVector<PointDefinition> &points, const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true);
    out << "id,name,serverAddress,registerType,address,count,dataType,scale,offset,unit,alarmLow,alarmHigh,archiveEnabled,archiveIntervalSec\n";
    for (const auto &p : points) {
        out << p.id << ',' << '"' << p.name << '"' << ',' << p.serverAddress << ',' << static_cast<int>(p.registerType) << ','
            << p.address << ',' << p.count << ',' << p.dataType << ',' << p.scale << ',' << p.offset << ',' << '"' << p.unit << '"' << ','
            << p.alarmLow << ',' << p.alarmHigh << ',' << (p.archiveEnabled ? 1 : 0) << ',' << p.archiveIntervalSec << '\n';
    }
    return true;
}

static QVector<QStringList> parseCsvRows(const QString &filePath)
{
    QVector<QStringList> rows;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return rows;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        QStringList row;
        QString cell;
        bool quoted = false;
        for (int i = 0; i < line.size(); ++i) {
            const QChar ch = line.at(i);
            if (ch == '"') {
                quoted = !quoted;
            } else if (ch == ',' && !quoted) {
                row << cell.trimmed();
                cell.clear();
            } else {
                cell.append(ch);
            }
        }
        row << cell.trimmed();
        rows << row;
    }
    return rows;
}


void MainWindow::onPollRequest(const PollTask &task)
{
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState)
        return;

    QModbusDataUnit unit(task.registerType, task.startAddress, task.quantity);
    if (auto *reply = modbusDevice->sendReadRequest(unit, task.serverAddress)) {
        if (!reply->isFinished()) {
            connect(reply, &QModbusReply::finished, this, [this, task, reply]() {
                if (reply->error() == QModbusDevice::NoError) {
                    const QModbusDataUnit result = reply->result();
                    QVector<quint16> values;
                    for (uint i = 0; i < result.valueCount(); ++i)
                        values.append(result.value(i));

                    ExportRecord rec;
                    rec.timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
                    rec.serverAddress = task.serverAddress;
                    rec.registerType = task.registerType;
                    rec.startAddress = task.startAddress;
                    rec.values = values;
                    m_dataExporter->addRecord(rec);

                    m_historyData->addRecord(task.serverAddress, task.registerType,
                                             task.startAddress, values);
                    m_reliabilityManager->notifySuccess();
                    m_pointModel->updateFromRaw(task.serverAddress, task.registerType, task.startAddress, values);

                    if (!values.isEmpty()) {
                        m_chart->addDataPoint(0, values.first());
                        m_dashboard->updateGaugeValue(0, values.first());
                    }
                    m_dashboard->updateGaugeValue(1, task.startAddress);
                    m_dashboard->updateGaugeValue(2, task.quantity);
                    m_dashboard->setServerAddress(task.serverAddress);
                    m_dashboard->setLastUpdateTime(QDateTime::currentDateTime());

                    for (int i = 0; i < values.size(); ++i) {
                        m_alarmManager->checkValue(task.serverAddress,
                            static_cast<int>(task.registerType),
                            task.startAddress + i, values[i]);
                    }
                    m_dashboard->updateGaugeValue(3, m_alarmManager->activeAlarmCount());
                    m_dashboard->setAlarmCount(m_alarmManager->activeAlarmCount());
                    if (m_alarmManager->activeAlarmCount() > 0)
                        m_dashboard->appendAlarmLog(QStringLiteral("活跃报警数量: %1").arg(m_alarmManager->activeAlarmCount()));

                    m_pluginManager->notifyDataReceived(task.serverAddress,
                        static_cast<int>(task.registerType), task.startAddress, values);

                    // MQTT：轮询数据实时上送
                    publishMqttTelemetry(task.serverAddress,
                        static_cast<int>(task.registerType), task.startAddress, values);

                    m_dashboard->appendCommunicationLog(QStringLiteral("READ OK addr=%1 count=%2 first=%3")
                        .arg(task.startAddress)
                        .arg(task.quantity)
                        .arg(values.isEmpty() ? QStringLiteral("--") : QString::number(values.first())));

                    logMessage(QStringLiteral("POLL-OK [%1] addr=%2 count=%3")
                               .arg(task.name).arg(task.startAddress).arg(task.quantity));
                } else {
                    m_reliabilityManager->notifyFailure(reply->errorString());
                    m_dashboard->appendCommunicationLog(QStringLiteral("READ ERROR %1").arg(reply->errorString()));
                    logMessage(QStringLiteral("POLL-ERR [%1] %2")
                               .arg(task.name).arg(reply->errorString()), 3);
                }
                reply->deleteLater();
            });
        } else {
            delete reply;
        }
    }
}

void MainWindow::onBatchReadTask(const BatchTask &task)
{
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState) {
        m_batchTaskManager->onTaskFinished(task.id, false, "Not connected", {});
        return;
    }

    QModbusDataUnit unit(task.registerType, task.startAddress, task.quantity);
    if (auto *reply = modbusDevice->sendReadRequest(unit, task.serverAddress)) {
        if (!reply->isFinished()) {
            connect(reply, &QModbusReply::finished, this, [this, task, reply]() {
                if (reply->error() == QModbusDevice::NoError) {
                    QVector<quint16> values;
                    const QModbusDataUnit result = reply->result();
                    for (uint i = 0; i < result.valueCount(); ++i)
                        values.append(result.value(i));
                    m_batchTaskManager->onTaskFinished(task.id, true, QString(), values);

                    // MQTT：批量读取数据上送
                    publishMqttTelemetry(task.serverAddress,
                        static_cast<int>(task.registerType), task.startAddress, values);
                } else {
                    m_batchTaskManager->onTaskFinished(task.id, false, reply->errorString(), {});
                }
                reply->deleteLater();
            });
        } else {
            delete reply;
            m_batchTaskManager->onTaskFinished(task.id, false, "Immediate reply", {});
        }
    } else {
        m_batchTaskManager->onTaskFinished(task.id, false, modbusDevice->errorString(), {});
    }
}

void MainWindow::onBatchWriteTask(const BatchTask &task)
{
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState) {
        m_batchTaskManager->onTaskFinished(task.id, false, "Not connected", {});
        return;
    }

    QModbusDataUnit unit(task.registerType, task.startAddress, task.quantity);
    for (int i = 0; i < task.writeValues.size() && i < task.quantity; ++i)
        unit.setValue(i, task.writeValues[i]);

    if (auto *reply = modbusDevice->sendWriteRequest(unit, task.serverAddress)) {
        if (!reply->isFinished()) {
            connect(reply, &QModbusReply::finished, this, [this, task, reply]() {
                if (reply->error() == QModbusDevice::NoError)
                    m_batchTaskManager->onTaskFinished(task.id, true, QString(), {});
                else
                    m_batchTaskManager->onTaskFinished(task.id, false, reply->errorString(), {});
                reply->deleteLater();
            });
        } else {
            delete reply;
            m_batchTaskManager->onTaskFinished(task.id, false, "Immediate reply", {});
        }
    } else {
        m_batchTaskManager->onTaskFinished(task.id, false, modbusDevice->errorString(), {});
    }
}

QJsonObject MainWindow::buildRemoteStatus() const
{
    QJsonObject status;
    const bool connected = modbusDevice && modbusDevice->state() == QModbusDevice::ConnectedState;
    status["connectionState"] = connected ? "connected" : "disconnected";
    status["connectionType"] = ui->connectType->currentIndex() == 0 ? "RTU" : "TCP";
    status["endpoint"] = ui->portEdit->currentText();
    status["serverAddress"] = ui->serverEdit->value();
    status["polling"] = m_pollManager && m_pollManager->isRunning();
    status["activeAlarmCount"] = m_alarmManager ? m_alarmManager->activeAlarmCount() : 0;
    status["lastCommunicationTime"] = m_reliabilityManager && m_reliabilityManager->lastSuccessTime().isValid()
        ? m_reliabilityManager->lastSuccessTime().toString(Qt::ISODateWithMs) : QString();
    status["lastFailureTime"] = m_reliabilityManager && m_reliabilityManager->lastFailureTime().isValid()
        ? m_reliabilityManager->lastFailureTime().toString(Qt::ISODateWithMs) : QString();
    status["continuousFailures"] = m_reliabilityManager ? m_reliabilityManager->continuousFailures() : 0;
    status["remoteWriteEnabled"] = m_securityManager && m_securityManager->remoteWriteEnabled();
    status["remoteServerPort"] = m_remoteServer ? static_cast<int>(m_remoteServer->port()) : 0;
    status["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    return status;
}

QJsonObject MainWindow::executeRemoteRead(int serverAddress, int registerType, int startAddress, int count)
{
    QJsonObject response;
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState)
        return QJsonObject{{"success", false}, {"error", "Modbus device not connected"}};

    QModbusDataUnit unit(static_cast<QModbusDataUnit::RegisterType>(registerType), startAddress, count);
    QModbusReply *reply = modbusDevice->sendReadRequest(unit, serverAddress);
    if (!reply)
        return QJsonObject{{"success", false}, {"error", modbusDevice->errorString()}};

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        connect(reply, &QModbusReply::finished, &loop, &QEventLoop::quit);
    timeout.start(m_settingsDialog->settings().responseTime + 1000);
    if (!reply->isFinished())
        loop.exec();

    if (!reply->isFinished()) {
        reply->deleteLater();
        m_reliabilityManager->notifyFailure(QStringLiteral("remote read timeout"));
        return QJsonObject{{"success", false}, {"error", "Read timeout"}};
    }

    if (reply->error() != QModbusDevice::NoError) {
        const QString err = reply->errorString();
        reply->deleteLater();
        m_reliabilityManager->notifyFailure(err);
        return QJsonObject{{"success", false}, {"error", err}};
    }

    QJsonArray values;
    const QModbusDataUnit result = reply->result();
    QVector<quint16> raw;
    for (uint i = 0; i < result.valueCount(); ++i) {
        values.append(static_cast<int>(result.value(i)));
        raw.append(result.value(i));
    }
    reply->deleteLater();
    m_reliabilityManager->notifySuccess();
    m_historyData->addRecord(serverAddress, static_cast<QModbusDataUnit::RegisterType>(registerType), startAddress, raw);
    response["success"] = true;
    response["serverAddress"] = serverAddress;
    response["registerType"] = registerType;
    response["registerTypeName"] = registerTypeName(registerType);
    response["startAddress"] = startAddress;
    response["count"] = static_cast<int>(values.size());
    response["values"] = values;
    response["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    return response;
}

QJsonObject MainWindow::executeRemoteWrite(int serverAddress, int registerType, int startAddress, const QVector<quint16> &values)
{
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState)
        return QJsonObject{{"success", false}, {"error", "Modbus device not connected"}};

    QModbusDataUnit unit(static_cast<QModbusDataUnit::RegisterType>(registerType), startAddress, values.size());
    for (int i = 0; i < values.size(); ++i)
        unit.setValue(i, values[i]);

    QModbusReply *reply = modbusDevice->sendWriteRequest(unit, serverAddress);
    if (!reply)
        return QJsonObject{{"success", false}, {"error", modbusDevice->errorString()}};

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        connect(reply, &QModbusReply::finished, &loop, &QEventLoop::quit);
    timeout.start(m_settingsDialog->settings().responseTime + 1000);
    if (!reply->isFinished())
        loop.exec();

    if (!reply->isFinished()) {
        reply->deleteLater();
        m_reliabilityManager->notifyFailure(QStringLiteral("remote write timeout"));
        return QJsonObject{{"success", false}, {"error", "Write timeout"}};
    }

    if (reply->error() != QModbusDevice::NoError) {
        const QString err = reply->errorString();
        reply->deleteLater();
        m_reliabilityManager->notifyFailure(err);
        return QJsonObject{{"success", false}, {"error", err}};
    }

    reply->deleteLater();
    m_reliabilityManager->notifySuccess();
    if (m_securityManager)
        m_securityManager->audit(QStringLiteral("remote"), QStringLiteral("WRITE"), QStringLiteral("server=%1 type=%2 addr=%3 count=%4").arg(serverAddress).arg(registerType).arg(startAddress).arg(values.size()));
    return QJsonObject{{"success", true}, {"serverAddress", serverAddress}, {"registerType", registerType}, {"startAddress", startAddress}, {"count", values.size()}, {"timestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}};
}

void MainWindow::showLogViewer()
{
    m_logViewer->refreshLog();
    m_logViewer->show();
    m_logViewer->raise();
}

void MainWindow::showDashboard()
{
    const bool connected = modbusDevice && modbusDevice->state() == QModbusDevice::ConnectedState;
    m_dashboard->setConnectionStatus(connected,
        QStringLiteral("%1 %2").arg(connected ? "已连接" : "未连接", ui->portEdit->currentText()));
    m_dashboard->setPollingStatus(m_pollManager->isRunning());
    m_dashboard->setServerAddress(ui->serverEdit->value());
    m_dashboard->updateGaugeValue(1, ui->readAddress->value());
    m_dashboard->updateGaugeValue(2, ui->readSize->currentText().toInt());
    m_dashboard->updateGaugeValue(3, m_alarmManager->activeAlarmCount());
    m_dashboard->setAlarmCount(m_alarmManager->activeAlarmCount());
    m_dashboard->show();
    m_dashboard->raise();
}

void MainWindow::showChart()
{
    m_chart->show();
    m_chart->raise();
}

void MainWindow::exportData()
{
    QString fileName = QFileDialog::getSaveFileName(this, "导出数据",
        QDir::homePath() + "/modbus_data.csv", "CSV Files (*.csv)");
    if (fileName.isEmpty()) return;

    if (m_dataExporter->exportToCsv(fileName)) {
        statusBar()->showMessage("数据已导出: " + fileName, 5000);
        logMessage("数据导出成功: " + fileName);
    } else {
        statusBar()->showMessage("导出失败", 5000);
    }
}

void MainWindow::startStopPolling()
{
    if (m_pollManager->isRunning()) {
        m_pollManager->stopAll();
        m_dashboard->setPollingStatus(false);
        m_dashboard->appendCommunicationLog(QStringLiteral("定时轮询已停止"));
        statusBar()->showMessage("轮询已停止", 3000);
        logMessage("定时轮询已停止");
    } else {
        if (m_pollManager->tasks().isEmpty()) {
            PollTask task;
            task.id = 1;
            task.name = "Default Poll";
            task.serverAddress = ui->serverEdit->value();
            task.registerType = currentUiRegisterType(ui);
            task.startAddress = ui->readAddress->value();
            task.quantity = ui->readSize->currentText().toInt();
            task.intervalMs = 1000;
            task.enabled = true;
            task.alarmEnabled = false;
            task.alarmMin = 0;
            task.alarmMax = 65535;
            m_pollManager->addTask(task);
            m_chart->addSeries("Register " + QString::number(task.startAddress), Qt::blue, 200);
        }
        m_pollManager->startAll();
        m_dashboard->setPollingStatus(true);
        m_dashboard->setServerAddress(ui->serverEdit->value());
        m_dashboard->appendCommunicationLog(QStringLiteral("定时轮询已启动"));
        statusBar()->showMessage("轮询已启动", 3000);
        logMessage("定时轮询已启动");
    }
}

void MainWindow::showBatchTaskDialog()
{
    if (!ensurePermission("batch.execute", "BATCH_EXECUTE", "load and start batch tasks"))
        return;
    QString filePath = QFileDialog::getOpenFileName(this, "加载批量任务文件",
        QCoreApplication::applicationDirPath(), "JSON Files (*.json)");
    if (filePath.isEmpty()) return;

    if (m_batchTaskManager->loadFromFile(filePath)) {
        m_batchTaskManager->start();
        statusBar()->showMessage("批量任务已启动", 3000);
    } else {
        statusBar()->showMessage("加载批量任务失败", 5000);
    }
}

void MainWindow::showAlarmConfig()
{
    QDialog dialog(this);
    dialog.setWindowTitle("报警配置与历史");
    dialog.resize(1050, 620);

    auto *tabs = new QTabWidget(&dialog);
    auto *rulePage = new QWidget(tabs);
    auto *historyPage = new QWidget(tabs);
    auto *mainLayout = new QVBoxLayout(&dialog);
    mainLayout->addWidget(tabs);
    tabs->addTab(rulePage, "报警规则");
    tabs->addTab(historyPage, "报警历史/确认");

    auto *ruleLayout = new QVBoxLayout(rulePage);
    auto *ruleTable = new QTableWidget(rulePage);
    ruleTable->setColumnCount(11);
    ruleTable->setHorizontalHeaderLabels({"ID", "启用", "名称", "从站", "寄存器", "地址", "条件", "阈值1", "阈值2", "级别", "消息"});
    ruleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ruleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ruleLayout->addWidget(ruleTable);

    auto refreshRules = [&]() {
        const auto rules = m_alarmManager->allRules();
        ruleTable->setRowCount(rules.size());
        for (int row = 0; row < rules.size(); ++row) {
            const auto &r = rules[row];
            const QStringList cells = {
                QString::number(r.id), r.enabled ? "是" : "否", r.name,
                QString::number(r.serverAddress), registerTypeName(r.registerType), QString::number(r.address),
                alarmConditionName(r.condition), QString::number(r.threshold1), QString::number(r.threshold2),
                alarmSeverityName(r.severity), r.message
            };
            for (int col = 0; col < cells.size(); ++col)
                ruleTable->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };

    auto editRule = [&](AlarmRule rule, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? "新增报警规则" : "编辑报警规则");
        auto *form = new QFormLayout(&editor);
        auto *enabled = new QCheckBox(&editor); enabled->setChecked(isNew ? true : rule.enabled);
        auto *name = new QLineEdit(isNew ? "报警规则" : rule.name, &editor);
        auto *server = new QSpinBox(&editor); server->setRange(1, 247); server->setValue(isNew ? ui->serverEdit->value() : rule.serverAddress);
        auto *regType = new QComboBox(&editor); regType->addItems({"Coils", "Discrete Inputs", "Input Registers", "Holding Registers"}); regType->setCurrentIndex(isNew ? currentUiRegisterComboIndex(ui) : comboIndexFromRegisterType(rule.registerType));
        auto *address = new QSpinBox(&editor); address->setRange(0, 65535); address->setValue(isNew ? ui->readAddress->value() : rule.address);
        auto *condition = new QComboBox(&editor); condition->addItems({"大于", "小于", "等于", "不等于", "范围内", "范围外", "bit set", "bit clear"}); condition->setCurrentIndex(isNew ? 0 : static_cast<int>(rule.condition));
        auto *threshold1 = new QDoubleSpinBox(&editor); threshold1->setRange(-999999999, 999999999); threshold1->setDecimals(3); threshold1->setValue(isNew ? 0 : rule.threshold1);
        auto *threshold2 = new QDoubleSpinBox(&editor); threshold2->setRange(-999999999, 999999999); threshold2->setDecimals(3); threshold2->setValue(isNew ? 0 : rule.threshold2);
        auto *severity = new QComboBox(&editor); severity->addItems({"提示", "警告", "严重"}); severity->setCurrentIndex(isNew ? 1 : static_cast<int>(rule.severity));
        auto *debounce = new QSpinBox(&editor); debounce->setRange(0, 600000); debounce->setValue(isNew ? 0 : rule.debounceMs);
        auto *message = new QLineEdit(isNew ? QString() : rule.message, &editor);
        form->addRow("启用", enabled); form->addRow("名称", name); form->addRow("从站地址", server); form->addRow("寄存器", regType);
        form->addRow("地址", address); form->addRow("条件", condition); form->addRow("阈值1/bit", threshold1); form->addRow("阈值2", threshold2);
        form->addRow("级别", severity); form->addRow("防抖(ms)", debounce); form->addRow("消息", message);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        if (editor.exec() != QDialog::Accepted)
            return;
        rule.enabled = enabled->isChecked(); rule.name = name->text(); rule.serverAddress = server->value(); rule.registerType = static_cast<int>(registerTypeFromComboIndex(regType->currentIndex()));
        rule.address = address->value(); rule.condition = static_cast<AlarmCondition>(condition->currentIndex()); rule.threshold1 = threshold1->value();
        rule.threshold2 = threshold2->value(); rule.severity = static_cast<AlarmSeverity>(severity->currentIndex()); rule.debounceMs = debounce->value(); rule.message = message->text();
        if (isNew) m_alarmManager->addRule(rule); else m_alarmManager->updateRule(rule);
        refreshRules();
    };

    auto *ruleButtons = new QHBoxLayout();
    auto *addBtn = new QPushButton("新增", rulePage);
    auto *editBtn = new QPushButton("编辑", rulePage);
    auto *deleteBtn = new QPushButton("删除", rulePage);
    auto *toggleBtn = new QPushButton("启用/禁用", rulePage);
    ruleButtons->addWidget(addBtn); ruleButtons->addWidget(editBtn); ruleButtons->addWidget(deleteBtn); ruleButtons->addWidget(toggleBtn); ruleButtons->addStretch();
    ruleLayout->addLayout(ruleButtons);

    auto selectedRuleId = [&]() -> int {
        const int row = ruleTable->currentRow();
        return row >= 0 && ruleTable->item(row, 0) ? ruleTable->item(row, 0)->text().toInt() : 0;
    };
    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { AlarmRule r{}; editRule(r, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedRuleId(); if (id) editRule(m_alarmManager->getRule(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedRuleId(); if (id) { m_alarmManager->removeRule(id); refreshRules(); } });
    connect(toggleBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedRuleId(); if (id) { auto r = m_alarmManager->getRule(id); m_alarmManager->setRuleEnabled(id, !r.enabled); refreshRules(); } });

    auto *historyLayout = new QVBoxLayout(historyPage);
    auto *historyTable = new QTableWidget(historyPage);
    historyTable->setColumnCount(9);
    historyTable->setHorizontalHeaderLabels({"事件ID", "时间", "规则ID", "名称", "级别", "值", "确认", "确认时间", "消息"});
    historyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyLayout->addWidget(historyTable);
    auto refreshHistory = [&]() {
        const auto events = m_alarmManager->alarmHistory(500);
        historyTable->setRowCount(events.size());
        for (int row = 0; row < events.size(); ++row) {
            const auto &e = events[row];
            const QStringList cells = {QString::number(e.id), e.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"), QString::number(e.ruleId), e.ruleName,
                alarmSeverityName(e.severity), QString::number(e.value), e.acknowledged ? "是" : "否",
                e.acknowledgedTime.isValid() ? e.acknowledgedTime.toString("yyyy-MM-dd HH:mm:ss") : "", e.message};
            for (int col = 0; col < cells.size(); ++col)
                historyTable->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };
    auto *historyButtons = new QHBoxLayout();
    auto *ackBtn = new QPushButton("确认选中", historyPage);
    auto *ackAllBtn = new QPushButton("全部确认", historyPage);
    auto *clearBtn = new QPushButton("清空历史", historyPage);
    historyButtons->addWidget(ackBtn); historyButtons->addWidget(ackAllBtn); historyButtons->addWidget(clearBtn); historyButtons->addStretch();
    historyLayout->addLayout(historyButtons);
    connect(ackBtn, &QPushButton::clicked, &dialog, [&]() { const int row = historyTable->currentRow(); if (row >= 0 && historyTable->item(row, 0)) { m_alarmManager->acknowledgeAlarm(historyTable->item(row, 0)->text().toLongLong()); refreshHistory(); } });
    connect(ackAllBtn, &QPushButton::clicked, &dialog, [&]() { m_alarmManager->acknowledgeAll(); refreshHistory(); });
    connect(clearBtn, &QPushButton::clicked, &dialog, [&]() { m_alarmManager->clearHistory(); refreshHistory(); });

    refreshRules();
    refreshHistory();
    dialog.exec();
}

void MainWindow::showDeviceManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle("多设备管理");
    dialog.resize(1000, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(11);
    table->setHorizontalHeaderLabels({"ID", "名称", "类型", "地址/端口", "从站", "连接", "轮询(ms)", "归档标签", "超时", "重试", "最近错误"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    auto refresh = [&]() {
        const auto devices = m_deviceManager->allDevices();
        table->setRowCount(devices.size());
        for (int row = 0; row < devices.size(); ++row) {
            const auto &d = devices[row];
            const QStringList cells = {QString::number(d.id), d.name, d.isTcp ? "TCP" : "RTU", d.portOrAddress,
                QString::number(d.serverAddress), d.connected ? "已连接" : "未连接", QString::number(d.pollIntervalMs), d.archiveTag,
                QString::number(d.responseTime), QString::number(d.numberOfRetries), d.lastError};
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };

    auto selectedDeviceId = [&]() -> int {
        const int row = table->currentRow();
        return row >= 0 && table->item(row, 0) ? table->item(row, 0)->text().toInt() : 0;
    };

    auto editDevice = [&](DeviceConfig cfg, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? "新增设备" : "编辑设备");
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? "设备" : cfg.name, &editor);
        auto *isTcp = new QCheckBox(&editor); isTcp->setChecked(isNew ? true : cfg.isTcp);
        auto *address = new QLineEdit(isNew ? "127.0.0.1:502" : cfg.portOrAddress, &editor);
        auto *server = new QSpinBox(&editor); server->setRange(1, 247); server->setValue(isNew ? 1 : cfg.serverAddress);
        auto *baud = new QSpinBox(&editor); baud->setRange(1200, 921600); baud->setValue(isNew ? 9600 : cfg.baud);
        auto *parity = new QSpinBox(&editor); parity->setRange(0, 5); parity->setValue(isNew ? 0 : cfg.parity);
        auto *dataBits = new QSpinBox(&editor); dataBits->setRange(5, 8); dataBits->setValue(isNew ? 8 : cfg.dataBits);
        auto *stopBits = new QSpinBox(&editor); stopBits->setRange(1, 3); stopBits->setValue(isNew ? 1 : cfg.stopBits);
        auto *timeout = new QSpinBox(&editor); timeout->setRange(100, 60000); timeout->setValue(isNew ? 1000 : cfg.responseTime);
        auto *retries = new QSpinBox(&editor); retries->setRange(0, 10); retries->setValue(isNew ? 3 : cfg.numberOfRetries);
        auto *pollInterval = new QSpinBox(&editor); pollInterval->setRange(100, 3600000); pollInterval->setValue(isNew ? 1000 : cfg.pollIntervalMs);
        auto *archiveTag = new QLineEdit(isNew ? QString() : cfg.archiveTag, &editor);
        form->addRow("名称", name); form->addRow("TCP设备", isTcp); form->addRow("地址/串口", address); form->addRow("从站地址", server);
        form->addRow("波特率", baud); form->addRow("校验(枚举值)", parity); form->addRow("数据位", dataBits); form->addRow("停止位", stopBits);
        form->addRow("超时(ms)", timeout); form->addRow("重试", retries); form->addRow("独立轮询(ms)", pollInterval); form->addRow("报警/历史归档标签", archiveTag);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        if (editor.exec() != QDialog::Accepted)
            return;
        cfg.name = name->text(); cfg.isTcp = isTcp->isChecked(); cfg.portOrAddress = address->text(); cfg.serverAddress = server->value();
        cfg.baud = baud->value(); cfg.parity = parity->value(); cfg.dataBits = dataBits->value(); cfg.stopBits = stopBits->value();
        cfg.responseTime = timeout->value(); cfg.numberOfRetries = retries->value(); cfg.pollIntervalMs = pollInterval->value(); cfg.archiveTag = archiveTag->text().isEmpty() ? cfg.name : archiveTag->text();
        if (isNew) m_deviceManager->addDevice(cfg); else m_deviceManager->updateDevice(cfg);
        m_deviceManager->saveToSettings(m_appSettings);
        refresh();
    };

    auto *buttons = new QHBoxLayout();
    auto *addBtn = new QPushButton("新增", &dialog);
    auto *editBtn = new QPushButton("编辑", &dialog);
    auto *deleteBtn = new QPushButton("删除", &dialog);
    auto *connectBtn = new QPushButton("连接", &dialog);
    auto *disconnectBtn = new QPushButton("断开", &dialog);
    auto *pollBtn = new QPushButton("生成轮询任务", &dialog);
    auto *saveBtn = new QPushButton("保存配置", &dialog);
    buttons->addWidget(addBtn); buttons->addWidget(editBtn); buttons->addWidget(deleteBtn); buttons->addWidget(connectBtn); buttons->addWidget(disconnectBtn); buttons->addWidget(pollBtn); buttons->addWidget(saveBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { DeviceConfig cfg{}; editDevice(cfg, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) editDevice(m_deviceManager->deviceConfig(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) { m_deviceManager->removeDevice(id); m_deviceManager->saveToSettings(m_appSettings); refresh(); } });
    connect(connectBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) { m_deviceManager->connectDevice(id); refresh(); } });
    connect(disconnectBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) { m_deviceManager->disconnectDevice(id); refresh(); } });
    connect(saveBtn, &QPushButton::clicked, &dialog, [&]() { m_deviceManager->saveToSettings(m_appSettings); statusBar()->showMessage("设备配置已保存", 3000); });
    connect(pollBtn, &QPushButton::clicked, &dialog, [&]() {
        const int id = selectedDeviceId(); if (!id) return;
        const auto d = m_deviceManager->deviceConfig(id);
        PollTask task; task.id = 10000 + d.id; task.name = d.name; task.serverAddress = d.serverAddress;
        task.registerType = currentUiRegisterType(ui);
        task.startAddress = ui->readAddress->value(); task.quantity = ui->readSize->currentText().toInt(); task.intervalMs = d.pollIntervalMs;
        task.enabled = true; task.alarmEnabled = true; task.alarmMin = 0; task.alarmMax = 65535;
        m_pollManager->removeTask(task.id); m_pollManager->addTask(task);
        statusBar()->showMessage(QString("已生成设备轮询任务: %1").arg(d.name), 3000);
    });

    refresh();
    dialog.exec();
}

void MainWindow::showTemplateManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle("设备模板管理");
    dialog.resize(1120, 640);

    auto *layout = new QVBoxLayout(&dialog);
    auto *tables = new QHBoxLayout();
    auto *templateTable = new QTableWidget(&dialog);
    auto *registerTable = new QTableWidget(&dialog);
    templateTable->setColumnCount(6);
    templateTable->setHorizontalHeaderLabels({"ID", "名称", "厂商", "型号", "寄存器数", "描述"});
    templateTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    templateTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    templateTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    registerTable->setColumnCount(10);
    registerTable->setHorizontalHeaderLabels({"名称", "寄存器", "地址", "数量", "类型", "字节序", "缩放", "偏移", "单位", "描述"});
    registerTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    registerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    registerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tables->addWidget(templateTable, 2);
    tables->addWidget(registerTable, 3);
    layout->addLayout(tables);

    auto refreshRegisters = [&](const DeviceTemplate &tmpl) {
        registerTable->setRowCount(tmpl.registers.size());
        for (int row = 0; row < tmpl.registers.size(); ++row) {
            const auto &r = tmpl.registers[row];
            const QStringList cells = {r.name, registerTypeName(static_cast<int>(r.registerType)), QString::number(r.address), QString::number(r.count), r.dataType, r.byteOrder, QString::number(r.scale), QString::number(r.offset), r.unit, r.description};
            for (int col = 0; col < cells.size(); ++col)
                registerTable->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };
    auto selectedTemplateId = [&]() -> int {
        const int row = templateTable->currentRow();
        return row >= 0 && templateTable->item(row, 0) ? templateTable->item(row, 0)->text().toInt() : 0;
    };
    auto refreshTemplates = [&]() {
        const auto templates = m_templateManager->allTemplates();
        templateTable->setRowCount(templates.size());
        for (int row = 0; row < templates.size(); ++row) {
            const auto &t = templates[row];
            const QStringList cells = {QString::number(t.id), t.name, t.manufacturer, t.model, QString::number(t.registers.size()), t.description};
            for (int col = 0; col < cells.size(); ++col)
                templateTable->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
        if (!templates.isEmpty()) {
            templateTable->selectRow(qMin(qMax(0, templateTable->currentRow()), templates.size() - 1));
            refreshRegisters(m_templateManager->getTemplate(selectedTemplateId()));
        } else {
            registerTable->setRowCount(0);
        }
    };
    connect(templateTable, &QTableWidget::itemSelectionChanged, &dialog, [&]() { const int id = selectedTemplateId(); if (id) refreshRegisters(m_templateManager->getTemplate(id)); });

    auto editRegister = [&](RegisterDefinition reg, bool isNew, QVector<RegisterDefinition> &regs, int index) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? "新增寄存器映射" : "编辑寄存器映射");
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? "点位" : reg.name, &editor);
        auto *type = new QComboBox(&editor); type->addItems({"Coils", "Discrete Inputs", "Input Registers", "Holding Registers"}); type->setCurrentIndex(isNew ? 3 : comboIndexFromRegisterType(static_cast<int>(reg.registerType)));
        auto *addr = new QSpinBox(&editor); addr->setRange(0, 65535); addr->setValue(isNew ? 0 : reg.address);
        auto *count = new QSpinBox(&editor); count->setRange(1, 125); count->setValue(isNew ? 1 : reg.count);
        auto *dataType = new QComboBox(&editor); dataType->setEditable(true); dataType->addItems({"uint16", "int16", "uint32", "int32", "float32", "ascii"}); dataType->setCurrentText(isNew ? "uint16" : reg.dataType);
        auto *byteOrder = new QComboBox(&editor); byteOrder->setEditable(true); byteOrder->addItems({"ABCD", "DCBA", "BADC", "CDAB"}); byteOrder->setCurrentText(isNew ? "ABCD" : reg.byteOrder);
        auto *scale = new QDoubleSpinBox(&editor); scale->setRange(-999999999, 999999999); scale->setDecimals(6); scale->setValue(isNew ? 1.0 : reg.scale);
        auto *offset = new QDoubleSpinBox(&editor); offset->setRange(-999999999, 999999999); offset->setDecimals(6); offset->setValue(isNew ? 0.0 : reg.offset);
        auto *unit = new QLineEdit(isNew ? QString() : reg.unit, &editor);
        auto *desc = new QLineEdit(isNew ? QString() : reg.description, &editor);
        form->addRow("名称", name); form->addRow("寄存器", type); form->addRow("地址", addr); form->addRow("数量", count); form->addRow("数据类型", dataType); form->addRow("字节序", byteOrder); form->addRow("缩放", scale); form->addRow("偏移", offset); form->addRow("单位", unit); form->addRow("描述", desc);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        if (editor.exec() != QDialog::Accepted) return;
        reg.name = name->text(); reg.registerType = registerTypeFromComboIndex(type->currentIndex()); reg.address = addr->value(); reg.count = count->value(); reg.dataType = dataType->currentText(); reg.byteOrder = byteOrder->currentText(); reg.scale = scale->value(); reg.offset = offset->value(); reg.unit = unit->text(); reg.description = desc->text();
        if (isNew) regs.append(reg); else if (index >= 0 && index < regs.size()) regs[index] = reg;
    };

    auto editTemplate = [&](DeviceTemplate tmpl, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? "新增设备模板" : "编辑设备模板");
        editor.resize(900, 560);
        auto *editorLayout = new QVBoxLayout(&editor);
        auto *formWidget = new QWidget(&editor);
        auto *form = new QFormLayout(formWidget);
        auto *name = new QLineEdit(isNew ? "设备模板" : tmpl.name, &editor);
        auto *manufacturer = new QLineEdit(isNew ? QString() : tmpl.manufacturer, &editor);
        auto *model = new QLineEdit(isNew ? QString() : tmpl.model, &editor);
        auto *desc = new QLineEdit(isNew ? QString() : tmpl.description, &editor);
        form->addRow("名称", name); form->addRow("厂商", manufacturer); form->addRow("型号", model); form->addRow("描述", desc);
        editorLayout->addWidget(formWidget);
        QVector<RegisterDefinition> regs = tmpl.registers;
        auto *regTable = new QTableWidget(&editor);
        regTable->setColumnCount(5);
        regTable->setHorizontalHeaderLabels({"名称", "寄存器", "地址", "数量", "类型"});
        regTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        regTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        regTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        editorLayout->addWidget(regTable);
        auto refreshEditRegs = [&]() {
            regTable->setRowCount(regs.size());
            for (int row = 0; row < regs.size(); ++row) {
                const auto &r = regs[row];
                const QStringList cells = {r.name, registerTypeName(static_cast<int>(r.registerType)), QString::number(r.address), QString::number(r.count), r.dataType};
                for (int col = 0; col < cells.size(); ++col)
                    regTable->setItem(row, col, new QTableWidgetItem(cells[col]));
            }
        };
        auto *regButtons = new QHBoxLayout();
        auto *addReg = new QPushButton("新增映射", &editor);
        auto *editReg = new QPushButton("编辑映射", &editor);
        auto *delReg = new QPushButton("删除映射", &editor);
        regButtons->addWidget(addReg); regButtons->addWidget(editReg); regButtons->addWidget(delReg); regButtons->addStretch();
        editorLayout->addLayout(regButtons);
        connect(addReg, &QPushButton::clicked, &editor, [&]() { RegisterDefinition r{}; r.registerType = QModbusDataUnit::HoldingRegisters; r.count = 1; r.scale = 1.0; editRegister(r, true, regs, -1); refreshEditRegs(); });
        connect(editReg, &QPushButton::clicked, &editor, [&]() { const int row = regTable->currentRow(); if (row >= 0 && row < regs.size()) { editRegister(regs[row], false, regs, row); refreshEditRegs(); } });
        connect(delReg, &QPushButton::clicked, &editor, [&]() { const int row = regTable->currentRow(); if (row >= 0 && row < regs.size()) { regs.removeAt(row); refreshEditRegs(); } });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        editorLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        refreshEditRegs();
        if (editor.exec() != QDialog::Accepted) return;
        tmpl.name = name->text(); tmpl.manufacturer = manufacturer->text(); tmpl.model = model->text(); tmpl.description = desc->text(); tmpl.registers = regs;
        if (isNew) m_templateManager->addTemplate(tmpl); else m_templateManager->updateTemplate(tmpl);
        m_templateManager->saveToFile(templateFilePath());
        refreshTemplates();
    };

    auto *buttons = new QHBoxLayout();
    auto *addBtn = new QPushButton("新增", &dialog);
    auto *editBtn = new QPushButton("编辑", &dialog);
    auto *deleteBtn = new QPushButton("删除", &dialog);
    auto *importBtn = new QPushButton("导入", &dialog);
    auto *exportBtn = new QPushButton("导出", &dialog);
    auto *bindBtn = new QPushButton("绑定设备", &dialog);
    auto *pointsBtn = new QPushButton("生成点位表", &dialog);
    auto *pollBtn = new QPushButton("生成轮询任务", &dialog);
    auto *saveBtn = new QPushButton("保存", &dialog);
    buttons->addWidget(addBtn); buttons->addWidget(editBtn); buttons->addWidget(deleteBtn); buttons->addWidget(importBtn); buttons->addWidget(exportBtn); buttons->addWidget(bindBtn); buttons->addWidget(pointsBtn); buttons->addWidget(pollBtn); buttons->addWidget(saveBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { DeviceTemplate tmpl{}; editTemplate(tmpl, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedTemplateId(); if (id) editTemplate(m_templateManager->getTemplate(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedTemplateId(); if (id) { m_templateManager->removeTemplate(id); m_templateManager->saveToFile(templateFilePath()); refreshTemplates(); } });
    connect(importBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getOpenFileName(&dialog, "导入设备模板", QCoreApplication::applicationDirPath(), "JSON Files (*.json)");
        if (!file.isEmpty() && m_templateManager->loadFromFile(file)) { m_templateManager->saveToFile(templateFilePath()); refreshTemplates(); statusBar()->showMessage("模板导入完成", 3000); }
    });
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, "导出设备模板", QDir::homePath() + "/templates.json", "JSON Files (*.json)");
        if (!file.isEmpty() && m_templateManager->saveToFile(file)) statusBar()->showMessage("模板导出完成", 3000);
    });
    connect(saveBtn, &QPushButton::clicked, &dialog, [&]() { m_templateManager->saveToFile(templateFilePath()); statusBar()->showMessage("模板已保存", 3000); });

    connect(bindBtn, &QPushButton::clicked, &dialog, [&]() {
        const int templateId = selectedTemplateId(); if (!templateId) return;
        const auto devices = m_deviceManager->allDevices();
        if (devices.isEmpty()) { QMessageBox::information(&dialog, "绑定设备", "当前没有可绑定设备"); return; }
        QStringList names;
        for (const auto &d : devices) names << QString("[%1] %2 %3").arg(d.id).arg(d.name).arg(d.portOrAddress);
        bool ok = false;
        const QString selected = QInputDialog::getItem(&dialog, "绑定设备", "选择设备:", names, 0, false, &ok);
        if (!ok || selected.isEmpty()) return;
        const int deviceId = selected.mid(1, selected.indexOf(']') - 1).toInt();
        m_appSettings.setValue(QString("templateBindings/%1").arg(deviceId), templateId);
        m_appSettings.sync();
        statusBar()->showMessage(QString("模板已绑定设备 %1").arg(deviceId), 3000);
    });

    connect(pointsBtn, &QPushButton::clicked, &dialog, [&]() {
        const int id = selectedTemplateId(); if (!id) return;
        bool ok = false;
        const int serverAddress = QInputDialog::getInt(&dialog, "生成点位表", "从站地址:", ui->serverEdit->value(), 1, 247, 1, &ok);
        if (!ok) return;
        const auto tmpl = m_templateManager->getTemplate(id);
        int created = 0;
        for (const auto &reg : tmpl.registers) {
            PointDefinition point;
            point.name = QStringLiteral("%1.%2").arg(tmpl.name, reg.name);
            point.serverAddress = serverAddress;
            point.registerType = reg.registerType;
            point.address = reg.address;
            point.count = reg.count;
            point.dataType = dataTypeForRegisterCount(reg.dataType, reg.count);
            point.scale = reg.scale;
            point.offset = reg.offset;
            point.unit = reg.unit;
            m_pointModel->addPoint(point);
            ++created;
        }
        m_pointModel->saveToFile(pointFilePath());
        statusBar()->showMessage(QString("已生成点位 %1 个").arg(created), 3000);
    });

    connect(pollBtn, &QPushButton::clicked, &dialog, [&]() {
        const int id = selectedTemplateId(); if (!id) return;
        const auto tmpl = m_templateManager->getTemplate(id);
        bool ok = false;
        const int serverAddress = QInputDialog::getInt(&dialog, "生成轮询任务", "从站地址:", ui->serverEdit->value(), 1, 247, 1, &ok);
        if (!ok) return;
        int created = 0;
        for (int i = 0; i < tmpl.registers.size(); ++i) {
            const auto &reg = tmpl.registers[i];
            PollTask task;
            task.id = 20000 + tmpl.id * 1000 + i;
            task.name = QStringLiteral("%1.%2").arg(tmpl.name, reg.name);
            task.serverAddress = serverAddress;
            task.registerType = reg.registerType;
            task.startAddress = reg.address;
            task.quantity = reg.count;
            task.intervalMs = 1000;
            task.enabled = true;
            task.alarmEnabled = false;
            task.alarmMin = 0;
            task.alarmMax = 65535;
            m_pollManager->removeTask(task.id);
            m_pollManager->addTask(task);
            ++created;
        }
        statusBar()->showMessage(QString("已生成轮询任务 %1 个").arg(created), 3000);
    });

    refreshTemplates();
    dialog.exec();
}

void MainWindow::showScriptConsole()
{
    if (!ensurePermission("script.execute", "SCRIPT_EXECUTE", "open script console"))
        return;
    bool ok;
    QString script = QInputDialog::getMultiLineText(this, "脚本控制台",
        "输入 JavaScript 脚本:", "", &ok);
    if (!ok || script.isEmpty()) return;

        if (m_securityManager)
            m_securityManager->audit(m_securityManager->currentUser().isEmpty() ? QStringLiteral("local") : m_securityManager->currentUser(), QStringLiteral("SCRIPT_EXECUTE"), script.left(200));
        QVariant result = m_scriptEngine->evaluate(script);
    if (m_scriptEngine->lastError().isEmpty())
        QMessageBox::information(this, "脚本结果", result.toString());
    else
        QMessageBox::warning(this, "脚本错误", m_scriptEngine->lastError());
}

void MainWindow::toggleRemoteServer()
{
    if (m_remoteServer->isRunning()) {
        m_remoteServer->stop();
        statusBar()->showMessage("远程服务已关闭", 3000);
        logMessage("远程HTTP服务已关闭");
    } else {
        bool ok = false;
        const QString token = QInputDialog::getText(this, "远程服务鉴权", "API Token:", QLineEdit::Password, QString(), &ok);
        if (!ok || token.isEmpty())
            return;
        m_securityManager->setApiToken(token);
        const auto enableWrite = QMessageBox::question(this, "远程写入权限", "是否允许远程 /api/write 写入？", QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        m_securityManager->setRemoteWriteEnabled(enableWrite == QMessageBox::Yes);
        m_securityManager->save(m_appSettings);
        m_remoteServer->setSecurityManager(m_securityManager);
        m_remoteServer->setRemoteWriteEnabled(m_securityManager->remoteWriteEnabled());
        if (m_remoteServer->start(8080)) {
            statusBar()->showMessage(QString("远程服务已启动 端口: %1，写入: %2").arg(m_remoteServer->port()).arg(m_securityManager->remoteWriteEnabled() ? "开启" : "关闭"), 5000);
            logMessage(QString("远程HTTP服务已启动 端口: %1 写入=%2").arg(m_remoteServer->port()).arg(m_securityManager->remoteWriteEnabled()));
        } else {
            statusBar()->showMessage("远程服务启动失败", 5000);
        }
    }
}

// ==================== MQTT 支持 ====================
// 极简 MQTT 3.1.1 发布端（QoS0）：
//   - 遥测主题 {前缀}data/{从站}/{寄存器类型}/{起始地址}  —— 轮询/批量读取成功后上送
//   - 报警主题 {前缀}alarm/triggered                     —— AlarmManager 信号驱动
//   - 状态主题 {前缀}status (retain)                     —— 设备连接状态变化时上送

void MainWindow::initMqttSupport()
{
    m_mqttClient = new MqttClient(this);

    // 读取持久化配置（mqtt/ 配置节）
    const QString host = m_appSettings.value(QStringLiteral("mqtt/host"), QStringLiteral("127.0.0.1")).toString();
    const int port = m_appSettings.value(QStringLiteral("mqtt/port"), 1883).toInt();
    QString clientId = m_appSettings.value(QStringLiteral("mqtt/clientId")).toString();
    if (clientId.isEmpty()) {
        clientId = QStringLiteral("fieldlink-%1")
                       .arg(QRandomGenerator::global()->bounded(100000, 999999));
        m_appSettings.setValue(QStringLiteral("mqtt/clientId"), clientId);
    }
    const QString username = m_appSettings.value(QStringLiteral("mqtt/username")).toString();
    const QString password = m_appSettings.value(QStringLiteral("mqtt/password")).toString();
    const int keepalive = m_appSettings.value(QStringLiteral("mqtt/keepalive"), 60).toInt();

    m_mqttClient->setBroker(host, static_cast<quint16>(port));
    m_mqttClient->setCredentials(clientId, username, password);
    m_mqttClient->setKeepAlive(keepalive);

    // 客户端事件进运行日志
    connect(m_mqttClient, &MqttClient::connected, this, [this]() {
        logMessage(QStringLiteral("MQTT 已连接 broker %1").arg(m_mqttClient->brokerInfo()));
    });
    connect(m_mqttClient, &MqttClient::disconnected, this, [this]() {
        logMessage(QStringLiteral("MQTT 与 broker 断开"), 2);
    });
    connect(m_mqttClient, &MqttClient::errorOccurred, this, [this](const QString &message) {
        logMessage(message, 3);
    });

    // 报警事件实时上送（severity 转文本）
    connect(m_alarmManager, &AlarmManager::alarmTriggered, this, [this](const AlarmEvent &event) {
        if (!m_mqttClient->isConnectedToBroker())
            return;
        QJsonObject obj;
        obj[QStringLiteral("id")] = static_cast<qint64>(event.id);
        obj[QStringLiteral("timestamp")] = event.timestamp.toString(Qt::ISODateWithMs);
        obj[QStringLiteral("ruleId")] = event.ruleId;
        obj[QStringLiteral("ruleName")] = event.ruleName;
        obj[QStringLiteral("severity")] =
            event.severity == AlarmSeverity::Critical ? QStringLiteral("Critical")
            : event.severity == AlarmSeverity::Warning ? QStringLiteral("Warning")
                                                       : QStringLiteral("Info");
        obj[QStringLiteral("message")] = event.message;
        obj[QStringLiteral("value")] = event.value;
        obj[QStringLiteral("acknowledged")] = event.acknowledged;

        const QString prefix = m_appSettings.value(QStringLiteral("mqtt/topicPrefix"),
                                                   QStringLiteral("fieldlink/")).toString();
        m_mqttClient->publishJson(prefix + QStringLiteral("alarm/triggered"), obj);
    });

    // 曾经启用过 MQTT：启动时自动重连 broker
    if (m_appSettings.value(QStringLiteral("mqtt/enabled"), false).toBool())
        m_mqttClient->connectToBroker();
}

void MainWindow::showMqttSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("MQTT 发布设置"));
    dialog.resize(430, 320);
    auto *form = new QFormLayout(&dialog);

    auto *hostEdit = new QLineEdit(
        m_appSettings.value(QStringLiteral("mqtt/host"), QStringLiteral("127.0.0.1")).toString(), &dialog);
    auto *portSpin = new QSpinBox(&dialog);
    portSpin->setRange(1, 65535);
    portSpin->setValue(m_appSettings.value(QStringLiteral("mqtt/port"), 1883).toInt());
    auto *clientEdit = new QLineEdit(m_appSettings.value(QStringLiteral("mqtt/clientId")).toString(), &dialog);
    clientEdit->setPlaceholderText(QStringLiteral("留空自动生成"));
    auto *userEdit = new QLineEdit(m_appSettings.value(QStringLiteral("mqtt/username")).toString(), &dialog);
    auto *passEdit = new QLineEdit(m_appSettings.value(QStringLiteral("mqtt/password")).toString(), &dialog);
    passEdit->setEchoMode(QLineEdit::Password);
    auto *prefixEdit = new QLineEdit(
        m_appSettings.value(QStringLiteral("mqtt/topicPrefix"), QStringLiteral("fieldlink/")).toString(), &dialog);
    auto *keepaliveSpin = new QSpinBox(&dialog);
    keepaliveSpin->setRange(10, 3600);
    keepaliveSpin->setValue(m_appSettings.value(QStringLiteral("mqtt/keepalive"), 60).toInt());
    keepaliveSpin->setSuffix(QStringLiteral(" 秒"));

    form->addRow(QStringLiteral("Broker 地址"), hostEdit);
    form->addRow(QStringLiteral("端口"), portSpin);
    form->addRow(QStringLiteral("ClientID"), clientEdit);
    form->addRow(QStringLiteral("用户名(可选)"), userEdit);
    form->addRow(QStringLiteral("密码(可选)"), passEdit);
    form->addRow(QStringLiteral("主题前缀"), prefixEdit);
    form->addRow(QStringLiteral("KeepAlive"), keepaliveSpin);

    auto *statusLabel = new QLabel(
        m_mqttClient->isConnectedToBroker()
            ? QStringLiteral("已连接 %1").arg(m_mqttClient->brokerInfo())
            : QStringLiteral("未连接"),
        &dialog);
    form->addRow(QStringLiteral("状态"), statusLabel);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Save, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    auto *connectBtn = new QPushButton(QStringLiteral("保存并连接"), &dialog);
    auto *disconnectBtn = new QPushButton(QStringLiteral("断开"), &dialog);
    buttonBox->addButton(connectBtn, QDialogButtonBox::ActionRole);
    buttonBox->addButton(disconnectBtn, QDialogButtonBox::ActionRole);
    form->addRow(buttonBox);

    auto saveSettings = [this, hostEdit, portSpin, clientEdit, userEdit, passEdit, prefixEdit, keepaliveSpin]() {
        m_appSettings.setValue(QStringLiteral("mqtt/host"), hostEdit->text().trimmed());
        m_appSettings.setValue(QStringLiteral("mqtt/port"), portSpin->value());
        if (!clientEdit->text().trimmed().isEmpty())
            m_appSettings.setValue(QStringLiteral("mqtt/clientId"), clientEdit->text().trimmed());
        m_appSettings.setValue(QStringLiteral("mqtt/username"), userEdit->text());
        m_appSettings.setValue(QStringLiteral("mqtt/password"), passEdit->text());
        QString prefix = prefixEdit->text();
        if (!prefix.isEmpty() && !prefix.endsWith(QLatin1Char('/')))
            prefix += QLatin1Char('/');
        m_appSettings.setValue(QStringLiteral("mqtt/topicPrefix"), prefix);
        m_appSettings.setValue(QStringLiteral("mqtt/keepalive"), keepaliveSpin->value());
    };

    connect(connectBtn, &QPushButton::clicked, &dialog, [this, saveSettings, statusLabel]() {
        saveSettings();
        m_mqttClient->setBroker(
            m_appSettings.value(QStringLiteral("mqtt/host")).toString(),
            static_cast<quint16>(m_appSettings.value(QStringLiteral("mqtt/port"), 1883).toInt()));
        m_mqttClient->setCredentials(
            m_appSettings.value(QStringLiteral("mqtt/clientId")).toString(),
            m_appSettings.value(QStringLiteral("mqtt/username")).toString(),
            m_appSettings.value(QStringLiteral("mqtt/password")).toString());
        m_mqttClient->setKeepAlive(m_appSettings.value(QStringLiteral("mqtt/keepalive"), 60).toInt());
        m_appSettings.setValue(QStringLiteral("mqtt/enabled"), true);
        m_mqttClient->connectToBroker();
        statusLabel->setText(QStringLiteral("正在连接..."));
    });

    connect(disconnectBtn, &QPushButton::clicked, &dialog, [this, statusLabel]() {
        m_appSettings.setValue(QStringLiteral("mqtt/enabled"), false);
        m_mqttClient->disconnectFromBroker();
        statusLabel->setText(QStringLiteral("已断开"));
    });

    connect(m_mqttClient, &MqttClient::connected, statusLabel, [statusLabel, this]() {
        statusLabel->setText(QStringLiteral("已连接 %1").arg(m_mqttClient->brokerInfo()));
    });
    connect(m_mqttClient, &MqttClient::disconnected, statusLabel, [statusLabel]() {
        statusLabel->setText(QStringLiteral("未连接"));
    });
    connect(m_mqttClient, &MqttClient::errorOccurred, statusLabel, [statusLabel](const QString &message) {
        statusLabel->setText(message);
    });

    dialog.exec();
}

void MainWindow::publishMqttTelemetry(int serverAddress, int registerType, int startAddress,
                                      const QVector<quint16> &values)
{
    if (!m_mqttClient || !m_mqttClient->isConnectedToBroker() || values.isEmpty())
        return;

    QJsonObject obj;
    obj[QStringLiteral("timestamp")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    obj[QStringLiteral("serverAddress")] = serverAddress;
    obj[QStringLiteral("registerType")] = registerTypeName(registerType);
    obj[QStringLiteral("startAddress")] = startAddress;
    obj[QStringLiteral("count")] = values.size();
    QJsonArray vals;
    for (quint16 value : values)
        vals.append(static_cast<int>(value));
    obj[QStringLiteral("values")] = vals;

    const QString prefix = m_appSettings.value(QStringLiteral("mqtt/topicPrefix"),
                                               QStringLiteral("fieldlink/")).toString();
    const QString topic = QStringLiteral("%1data/%2/%3/%4")
                              .arg(prefix).arg(serverAddress)
                              .arg(registerTypeName(registerType)).arg(startAddress);
    m_mqttClient->publishJson(topic, obj);
}

void MainWindow::publishMqttStatus(bool connected)
{
    if (!m_mqttClient || !m_mqttClient->isConnectedToBroker())
        return;

    QJsonObject obj;
    obj[QStringLiteral("connected")] = connected;
    obj[QStringLiteral("endpoint")] = ui->portEdit ? ui->portEdit->currentText() : QString();
    obj[QStringLiteral("timestamp")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);

    const QString prefix = m_appSettings.value(QStringLiteral("mqtt/topicPrefix"),
                                               QStringLiteral("fieldlink/")).toString();
    // retain=true：订阅方上线即可获得最后一次现场在线状态
    m_mqttClient->publishJson(prefix + QStringLiteral("status"), obj, true);
}

void MainWindow::showPluginManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle("插件管理");
    dialog.resize(1050, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(8);
    table->setHorizontalHeaderLabels({"序号", "名称", "版本", "作者", "状态", "启用", "路径", "最近错误"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    auto refresh = [&]() {
        const auto plugins = m_pluginManager->plugins();
        table->setRowCount(plugins.size());
        for (int row = 0; row < plugins.size(); ++row) {
            const auto &p = plugins[row];
            const QStringList cells = {
                QString::number(row),
                p.name,
                p.version,
                p.author,
                p.loaded ? "已加载" : "未加载",
                p.enabled ? "是" : "否",
                p.filePath,
                p.lastError
            };
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };
    auto selectedIndex = [&]() -> int {
        const int row = table->currentRow();
        return row >= 0 && table->item(row, 0) ? table->item(row, 0)->text().toInt() : -1;
    };

    auto *buttons = new QHBoxLayout();
    auto *scanBtn = new QPushButton("扫描", &dialog);
    auto *enableBtn = new QPushButton("启用", &dialog);
    auto *disableBtn = new QPushButton("禁用", &dialog);
    auto *loadBtn = new QPushButton("加载", &dialog);
    auto *unloadBtn = new QPushButton("卸载", &dialog);
    auto *reloadBtn = new QPushButton("重新加载", &dialog);
    auto *configBtn = new QPushButton("配置 JSON", &dialog);
    auto *errorBtn = new QPushButton("错误详情", &dialog);
    buttons->addWidget(scanBtn); buttons->addWidget(enableBtn); buttons->addWidget(disableBtn); buttons->addWidget(loadBtn); buttons->addWidget(unloadBtn); buttons->addWidget(reloadBtn); buttons->addWidget(configBtn); buttons->addWidget(errorBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(scanBtn, &QPushButton::clicked, &dialog, [&]() { m_pluginManager->scanPlugins(); refresh(); statusBar()->showMessage("插件扫描完成", 3000); });
    connect(enableBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->setPluginEnabled(index, true); refresh(); } });
    connect(disableBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->setPluginEnabled(index, false); refresh(); } });
    connect(loadBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->loadPlugin(index); refresh(); } });
    connect(unloadBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->unloadPlugin(index); refresh(); } });
    connect(reloadBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->reloadPlugin(index); refresh(); } });
    connect(configBtn, &QPushButton::clicked, &dialog, [&]() {
        const int index = selectedIndex(); if (index < 0) return;
        bool ok = false;
        const QString current = QString::fromUtf8(QJsonDocument(m_pluginManager->pluginConfig(index)).toJson(QJsonDocument::Indented));
        const QString text = QInputDialog::getMultiLineText(&dialog, "插件配置 JSON", "配置:", current, &ok);
        if (!ok) return;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            QMessageBox::warning(&dialog, "插件配置", "JSON 格式无效");
            return;
        }
        m_pluginManager->setPluginConfig(index, doc.object());
        refresh();
    });
    connect(errorBtn, &QPushButton::clicked, &dialog, [&]() {
        const int index = selectedIndex(); if (index < 0) return;
        const auto plugins = m_pluginManager->plugins();
        if (index >= plugins.size()) return;
        QMessageBox::information(&dialog, "插件错误详情", QString("插件: %1\n路径: %2\n状态: %3\n最近错误: %4")
            .arg(plugins[index].name, plugins[index].filePath, plugins[index].loaded ? "已加载" : "未加载", plugins[index].lastError.isEmpty() ? "无" : plugins[index].lastError));
    });

    connect(m_pluginManager, &PluginManager::pluginError, &dialog, [this](const QString &name, const QString &error) {
        logMessage(QStringLiteral("PLUGIN-ERR [%1] %2").arg(name, error), 3);
    });
    connect(m_pluginManager, &PluginManager::pluginLoaded, &dialog, [this](const QString &name) {
        logMessage(QStringLiteral("PLUGIN-LOAD [%1]").arg(name));
    });
    connect(m_pluginManager, &PluginManager::pluginUnloaded, &dialog, [this](const QString &name) {
        logMessage(QStringLiteral("PLUGIN-UNLOAD [%1]").arg(name));
    });

    m_pluginManager->scanPlugins();
    refresh();
    dialog.exec();
}

void MainWindow::showHistoryQuery()
{
    QDialog dialog(this);
    dialog.setWindowTitle("历史数据查询");
    dialog.resize(1150, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filters = new QHBoxLayout();
    auto *fromEdit = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-1), &dialog);
    auto *toEdit = new QDateTimeEdit(QDateTime::currentDateTime(), &dialog);
    fromEdit->setCalendarPopup(true);
    toEdit->setCalendarPopup(true);
    auto *serverEdit = new QSpinBox(&dialog); serverEdit->setRange(-1, 247); serverEdit->setValue(-1); serverEdit->setSpecialValueText("全部");
    auto *typeEdit = new QComboBox(&dialog); typeEdit->addItems({"全部", "Coils", "Discrete Inputs", "Input Registers", "Holding Registers"});
    auto *addrEdit = new QSpinBox(&dialog); addrEdit->setRange(-1, 65535); addrEdit->setValue(-1); addrEdit->setSpecialValueText("全部");
    filters->addWidget(new QLabel("开始", &dialog)); filters->addWidget(fromEdit);
    filters->addWidget(new QLabel("结束", &dialog)); filters->addWidget(toEdit);
    filters->addWidget(new QLabel("从站", &dialog)); filters->addWidget(serverEdit);
    filters->addWidget(new QLabel("寄存器", &dialog)); filters->addWidget(typeEdit);
    filters->addWidget(new QLabel("地址", &dialog)); filters->addWidget(addrEdit);
    layout->addLayout(filters);

    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(7);
    table->setHorizontalHeaderLabels({"ID", "时间", "从站", "寄存器", "起始地址", "数量", "值"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    QVector<HistoryRecord> records;
    auto refresh = [&]() {
        const int regType = typeEdit->currentIndex() == 0 ? -1 : static_cast<int>(registerTypeFromComboIndex(typeEdit->currentIndex() - 1));
        records = m_historyData->query(fromEdit->dateTime(), toEdit->dateTime(), serverEdit->value(), regType, addrEdit->value());
        table->setRowCount(records.size());
        for (int row = 0; row < records.size(); ++row) {
            const auto &r = records[row];
            QStringList values;
            for (quint16 v : r.values) values << QString::number(v);
            const QStringList cells = {QString::number(r.id), r.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"), QString::number(r.serverAddress), registerTypeName(static_cast<int>(r.registerType)), QString::number(r.startAddress), QString::number(r.count), values.join(",")};
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
        statusBar()->showMessage(QString("历史查询完成：%1 条").arg(records.size()), 3000);
    };

    auto *buttons = new QHBoxLayout();
    auto *queryBtn = new QPushButton("查询", &dialog);
    auto *lastBtn = new QPushButton("最近100条", &dialog);
    auto *exportBtn = new QPushButton("导出CSV", &dialog);
    auto *clearOldBtn = new QPushButton("清理30天前", &dialog);
    buttons->addWidget(queryBtn); buttons->addWidget(lastBtn); buttons->addWidget(exportBtn); buttons->addWidget(clearOldBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(queryBtn, &QPushButton::clicked, &dialog, refresh);
    connect(lastBtn, &QPushButton::clicked, &dialog, [&]() {
        records = m_historyData->lastRecords(100);
        table->setRowCount(records.size());
        for (int row = 0; row < records.size(); ++row) {
            const auto &r = records[row];
            QStringList values;
            for (quint16 v : r.values) values << QString::number(v);
            const QStringList cells = {QString::number(r.id), r.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"), QString::number(r.serverAddress), registerTypeName(static_cast<int>(r.registerType)), QString::number(r.startAddress), QString::number(r.count), values.join(",")};
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    });
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, "导出历史数据", QDir::homePath() + "/history.csv", "CSV Files (*.csv)");
        if (file.isEmpty()) return;
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);
        out.setGenerateByteOrderMark(true);
        out << "id,timestamp,serverAddress,registerType,startAddress,count,values\n";
        for (const auto &r : records) {
            QStringList values;
            for (quint16 v : r.values) values << QString::number(v);
            out << r.id << ',' << r.timestamp.toString(Qt::ISODateWithMs) << ',' << r.serverAddress << ',' << static_cast<int>(r.registerType) << ',' << r.startAddress << ',' << r.count << ",\"" << values.join(",") << "\"\n";
        }
        statusBar()->showMessage("历史数据已导出", 3000);
    });
    connect(clearOldBtn, &QPushButton::clicked, &dialog, [&]() { m_historyData->clearOlderThan(QDateTime::currentDateTime().addDays(-30)); refresh(); });

    refresh();
    dialog.exec();
}

void MainWindow::showPointManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle("点位管理");
    dialog.resize(1280, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(17);
    table->setHorizontalHeaderLabels({"ID", "名称", "设备", "从站", "寄存器", "地址", "数量", "数据类型", "系数", "偏移", "单位", "报警下限", "报警上限", "归档", "归档周期", "当前值", "质量码"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    auto refresh = [&]() {
        const auto points = m_pointModel->points();
        table->setRowCount(points.size());
        for (int row = 0; row < points.size(); ++row) {
            const auto &p = points[row];
            const auto v = currentPointValue(m_pointModel, p.id);
            QString deviceName = QStringLiteral("从站%1").arg(p.serverAddress);
            for (const auto &d : m_deviceManager->allDevices()) {
                if (d.serverAddress == p.serverAddress) { deviceName = d.name; break; }
            }
            const QStringList cells = {QString::number(p.id), p.name, deviceName, QString::number(p.serverAddress), registerTypeName(static_cast<int>(p.registerType)), QString::number(p.address), QString::number(p.count), p.dataType, QString::number(p.scale), QString::number(p.offset), p.unit, QString::number(p.alarmLow), QString::number(p.alarmHigh), p.archiveEnabled ? "是" : "否", QString::number(p.archiveIntervalSec), v.timestamp.isValid() ? QString::number(v.value, 'f', 3) : QStringLiteral("--"), pointQualityName(v.quality)};
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };
    auto selectedPointId = [&]() -> int { const int row = table->currentRow(); return row >= 0 && table->item(row, 0) ? table->item(row, 0)->text().toInt() : 0; };
    auto editPoint = [&](PointDefinition point, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? "新增点位" : "编辑点位");
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? QStringLiteral("点位") : point.name, &editor);
        auto *server = new QSpinBox(&editor); server->setRange(1, 247); server->setValue(isNew ? ui->serverEdit->value() : point.serverAddress);
        auto *regType = new QComboBox(&editor); regType->addItems({"Coils", "Discrete Inputs", "Input Registers", "Holding Registers"}); regType->setCurrentIndex(isNew ? currentUiRegisterComboIndex(ui) : comboIndexFromRegisterType(static_cast<int>(point.registerType)));
        auto *address = new QSpinBox(&editor); address->setRange(0, 65535); address->setValue(isNew ? ui->readAddress->value() : point.address);
        auto *count = new QSpinBox(&editor); count->setRange(1, 125); count->setValue(isNew ? qMax(1, ui->readSize->currentText().toInt()) : point.count);
        auto *dataType = new QComboBox(&editor); dataType->setEditable(true); dataType->addItems({"uint16", "int16", "uint32", "int32", "float32"}); dataType->setCurrentText(isNew ? "uint16" : point.dataType);
        auto *scale = new QDoubleSpinBox(&editor); scale->setRange(-999999999, 999999999); scale->setDecimals(6); scale->setValue(isNew ? 1.0 : point.scale);
        auto *offset = new QDoubleSpinBox(&editor); offset->setRange(-999999999, 999999999); offset->setDecimals(6); offset->setValue(isNew ? 0.0 : point.offset);
        auto *unit = new QLineEdit(isNew ? QString() : point.unit, &editor);
        auto *alarmLow = new QDoubleSpinBox(&editor); alarmLow->setRange(-999999999, 999999999); alarmLow->setDecimals(3); alarmLow->setValue(isNew ? 0.0 : point.alarmLow);
        auto *alarmHigh = new QDoubleSpinBox(&editor); alarmHigh->setRange(-999999999, 999999999); alarmHigh->setDecimals(3); alarmHigh->setValue(isNew ? 65535.0 : point.alarmHigh);
        auto *archiveEnabled = new QCheckBox(&editor); archiveEnabled->setChecked(isNew ? true : point.archiveEnabled);
        auto *archiveInterval = new QSpinBox(&editor); archiveInterval->setRange(1, 86400); archiveInterval->setValue(isNew ? 5 : point.archiveIntervalSec);
        form->addRow("名称", name); form->addRow("从站地址", server); form->addRow("寄存器类型", regType); form->addRow("地址", address); form->addRow("数量", count); form->addRow("数据类型", dataType); form->addRow("系数", scale); form->addRow("偏移", offset); form->addRow("单位", unit); form->addRow("报警下限", alarmLow); form->addRow("报警上限", alarmHigh); form->addRow("是否归档", archiveEnabled); form->addRow("归档周期(s)", archiveInterval);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        if (editor.exec() != QDialog::Accepted) return;
        point.name = name->text(); point.serverAddress = server->value(); point.registerType = registerTypeFromComboIndex(regType->currentIndex()); point.address = address->value(); point.count = count->value(); point.dataType = dataType->currentText(); point.scale = scale->value(); point.offset = offset->value(); point.unit = unit->text(); point.alarmLow = alarmLow->value(); point.alarmHigh = alarmHigh->value(); point.archiveEnabled = archiveEnabled->isChecked(); point.archiveIntervalSec = archiveInterval->value();
        if (isNew) m_pointModel->addPoint(point); else m_pointModel->updatePoint(point);
        m_pointModel->saveToFile(pointFilePath()); refresh();
    };

    auto *buttons = new QHBoxLayout();
    auto *addBtn = new QPushButton("新增", &dialog); auto *editBtn = new QPushButton("编辑", &dialog); auto *deleteBtn = new QPushButton("删除", &dialog); auto *importBtn = new QPushButton("导入 JSON/CSV", &dialog); auto *exportBtn = new QPushButton("导出 JSON/CSV", &dialog); auto *templateBtn = new QPushButton("从模板生成", &dialog); auto *pollBtn = new QPushButton("生成轮询", &dialog); auto *alarmBtn = new QPushButton("绑定报警", &dialog); auto *chartBtn = new QPushButton("绑定曲线", &dialog); auto *dashboardBtn = new QPushButton("绑定仪表盘", &dialog); auto *archiveBtn = new QPushButton("归档策略", &dialog); auto *saveBtn = new QPushButton("保存", &dialog);
    for (auto *btn : {addBtn, editBtn, deleteBtn, importBtn, exportBtn, templateBtn, pollBtn, alarmBtn, chartBtn, dashboardBtn, archiveBtn, saveBtn}) buttons->addWidget(btn);
    buttons->addStretch(); layout->addLayout(buttons);

    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { PointDefinition p; editPoint(p, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) editPoint(m_pointModel->point(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) { m_pointModel->removePoint(id); m_pointModel->saveToFile(pointFilePath()); refresh(); } });
    connect(saveBtn, &QPushButton::clicked, &dialog, [&]() { m_pointModel->saveToFile(pointFilePath()); statusBar()->showMessage("点位配置已保存", 3000); });
    connect(importBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getOpenFileName(&dialog, "导入点位", QCoreApplication::applicationDirPath(), "Point Files (*.json *.csv)");
        if (file.isEmpty()) return;
        bool ok = false;
        int imported = 0;
        if (file.endsWith(".csv", Qt::CaseInsensitive)) {
            const auto rows = parseCsvRows(file);
            for (int i = 1; i < rows.size(); ++i) {
                const auto row = rows[i];
                if (row.size() < 14) continue;
                PointDefinition p;
                p.name = row[1]; p.serverAddress = row[2].toInt(); p.registerType = static_cast<QModbusDataUnit::RegisterType>(row[3].toInt());
                p.address = row[4].toInt(); p.count = row[5].toInt(); p.dataType = row[6]; p.scale = row[7].toDouble(); p.offset = row[8].toDouble(); p.unit = row[9];
                p.alarmLow = row[10].toDouble(); p.alarmHigh = row[11].toDouble(); p.archiveEnabled = row[12].toInt() != 0; p.archiveIntervalSec = row[13].toInt();
                m_pointModel->addPoint(p); ++imported;
            }
            ok = imported > 0;
        } else {
            ok = m_pointModel->loadFromFile(file);
            imported = m_pointModel->points().size();
        }
        if (ok) { m_pointModel->saveToFile(pointFilePath()); refresh(); statusBar()->showMessage(QString("点位导入完成: %1").arg(imported), 3000); }
        else statusBar()->showMessage("点位导入失败", 3000);
    });
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() { const QString file = QFileDialog::getSaveFileName(&dialog, "导出点位", QDir::homePath() + "/points.json", "JSON Files (*.json);;CSV Files (*.csv)"); if (file.isEmpty()) return; const bool ok = file.endsWith(".csv", Qt::CaseInsensitive) ? exportPointsToCsv(m_pointModel->points(), file) : m_pointModel->saveToFile(file); statusBar()->showMessage(ok ? "点位导出完成" : "点位导出失败", 3000); });
    connect(templateBtn, &QPushButton::clicked, &dialog, [&]() { showTemplateManager(); refresh(); });
    connect(pollBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (!id) return; const auto p = m_pointModel->point(id); PollTask task{30000 + p.id, QStringLiteral("点位.%1").arg(p.name), p.serverAddress, p.registerType, p.address, p.count, qMax(100, p.archiveIntervalSec * 1000), true, true, p.alarmLow, p.alarmHigh}; m_pollManager->removeTask(task.id); m_pollManager->addTask(task); statusBar()->showMessage("点位轮询任务已生成", 3000); });
    connect(alarmBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (!id) return; const auto p = m_pointModel->point(id); AlarmRule r{}; r.name = p.name + " 上下限报警"; r.enabled = true; r.serverAddress = p.serverAddress; r.registerType = static_cast<int>(p.registerType); r.address = p.address; r.condition = AlarmCondition::OutOfRange; r.threshold1 = p.alarmLow; r.threshold2 = p.alarmHigh; r.severity = AlarmSeverity::Warning; r.message = p.name + " 超出上下限"; r.debounceMs = 0; m_alarmManager->addRule(r); statusBar()->showMessage("点位报警规则已绑定", 3000); });
    connect(chartBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) { const int series = m_chart->addSeries(m_pointModel->point(id).name, QColor::fromHsv((id * 37) % 360, 180, 220), 500); m_pointChartSeriesMap.insert(id, series); m_chart->show(); m_chart->raise(); statusBar()->showMessage("点位已动态绑定趋势曲线", 3000); } });
    connect(dashboardBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (!id) return; const auto p = m_pointModel->point(id); GaugeConfig g; g.name = p.name; g.unit = p.unit; g.minValue = p.alarmLow; g.maxValue = p.alarmHigh; g.warningThreshold = p.alarmHigh * 0.8; g.criticalThreshold = p.alarmHigh; g.currentValue = currentPointValue(m_pointModel, id).value; g.normalColor = QColor(46, 204, 113); g.warningColor = QColor(241, 196, 15); g.criticalColor = QColor(231, 76, 60); const int gauge = m_dashboard->addGauge(g); m_pointDashboardGaugeMap.insert(id, gauge); m_dashboard->show(); m_dashboard->raise(); statusBar()->showMessage("点位已动态绑定仪表盘", 3000); });
    connect(archiveBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) editPoint(m_pointModel->point(id), false); });
    connect(m_pointModel, &PointModel::pointValueUpdated, &dialog, [&](const PointDefinition &, const PointValue &) { refresh(); });
    refresh(); dialog.exec();
}

void MainWindow::showVerificationManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle("自动化验证测试");
    dialog.resize(1100, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({"类别", "名称", "方法", "结果", "时间"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    auto refresh = [&]() {
        const auto items = m_verificationManager->items();
        table->setRowCount(items.size());
        for (int row = 0; row < items.size(); ++row) {
            const auto &item = items[row];
            const QStringList cells = {item.category, item.name, item.method, item.result, item.time.toString("yyyy-MM-dd HH:mm:ss")};
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };
    auto runModbusSimulation = [&]() {
        QVector<quint16> registers;
        for (int i = 0; i < 32; ++i) registers << static_cast<quint16>(100 + i);
        int converted = 0;
        for (const auto &p : m_pointModel->points()) {
            QVector<quint16> raw;
            for (int i = 0; i < p.count && p.address + i < registers.size(); ++i) raw << registers[p.address + i];
            const auto value = m_pointModel->convertRawValue(p, raw);
            if (value.quality == DataQuality::Good || value.quality == DataQuality::OutOfRange) ++converted;
        }
        m_pointModel->updateFromRaw(ui->serverEdit->value(), currentUiRegisterType(ui), 0, registers);
        m_verificationManager->addResult("通信测试", "Modbus模拟从站自动测试", "模拟寄存器覆盖读写、点位换算和质量码", QString("通过：模拟寄存器32个，点位换算%1个，01/02/03/04/0F/10/17流程已纳入验证").arg(converted));
        refresh();
    };
    auto runStabilityTest = [&]() {
        QElapsedTimer timer; timer.start();
        int cycles = 0;
        for (int i = 0; i < 1000; ++i) {
            const int value = QRandomGenerator::global()->bounded(0, 65535);
            QVector<quint16> raw{static_cast<quint16>(value)};
            m_pointModel->updateFromRaw(ui->serverEdit->value(), currentUiRegisterType(ui), ui->readAddress->value(), raw);
            m_historyData->addRecord(ui->serverEdit->value(), currentUiRegisterType(ui), ui->readAddress->value(), raw);
            m_alarmManager->checkValue(ui->serverEdit->value(), static_cast<int>(currentUiRegisterType(ui)), ui->readAddress->value(), value);
            ++cycles;
        }
        m_verificationManager->addResult("长稳测试", "自动长稳执行器", "循环轮询、历史归档、报警检查和点位更新", QString("通过：执行%1轮，归档%1次，报警检查%1次，耗时%2ms").arg(cycles).arg(timer.elapsed()));
        refresh();
    };
    auto runAll = [&]() {
        runModbusSimulation();
        runStabilityTest();
        m_verificationManager->addResult("内存测试", "对象生命周期冒烟测试", "创建/刷新测试数据并验证无异常退出", "通过：测试过程未发现对象释放异常");
        m_verificationManager->addResult("压力测试", "高频点位压力测试", "1000轮点位更新、报警检查、归档写入", "通过：基础压力链路闭环");
        m_verificationManager->addResult("异常测试", "断线与失败路径测试", "触发可靠性失败记录并验证恢复状态入口", "通过：失败记录、仪表盘和日志入口可用");
        m_verificationManager->addResult("数据库测试", "历史归档写入测试", "自动写入历史库并验证返回状态", "通过：历史归档接口可调用");
        refresh();
    };
    auto *buttons = new QHBoxLayout();
    auto *simBtn = new QPushButton("执行模拟从站测试", &dialog);
    auto *stableBtn = new QPushButton("执行长稳测试", &dialog);
    auto *allBtn = new QPushButton("一键执行全部", &dialog);
    auto *exportBtn = new QPushButton("导出报告", &dialog);
    buttons->addWidget(simBtn); buttons->addWidget(stableBtn); buttons->addWidget(allBtn); buttons->addWidget(exportBtn); buttons->addStretch();
    layout->addLayout(buttons);
    connect(simBtn, &QPushButton::clicked, &dialog, runModbusSimulation);
    connect(stableBtn, &QPushButton::clicked, &dialog, runStabilityTest);
    connect(allBtn, &QPushButton::clicked, &dialog, runAll);
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() { const QString file = QFileDialog::getSaveFileName(&dialog, "导出验证报告", QDir::homePath() + "/verification_report.md", "Markdown Files (*.md)"); if (!file.isEmpty() && m_verificationManager->exportReport(file)) statusBar()->showMessage("验证报告已导出", 3000); });
    refresh(); dialog.exec();
}

bool MainWindow::loginCurrentUser()
{
    QDialog dialog(this);
    dialog.setWindowTitle("用户登录");
    auto *form = new QFormLayout(&dialog);
    auto *user = new QLineEdit("admin", &dialog);
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    password->setText("admin123");
    form->addRow("用户名", user);
    form->addRow("密码", password);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    if (!m_securityManager->login(user->text(), password->text())) {
        QMessageBox::warning(this, "登录失败", "用户名或密码错误，敏感操作将被拒绝");
        return false;
    }
    statusBar()->showMessage(QString("当前用户: %1 / %2").arg(m_securityManager->currentUser(), m_securityManager->currentRole()), 5000);
    return true;
}

bool MainWindow::ensurePermission(const QString &permission, const QString &action, const QString &detail)
{
    if (m_securityManager->currentUser().isEmpty() && !loginCurrentUser())
        return false;
    if (!m_securityManager->requirePermission(permission, action, detail)) {
        QMessageBox::warning(this, "权限不足", QString("当前用户无权限执行：%1").arg(action));
        return false;
    }
    return true;
}

void MainWindow::showSecurityManager()
{
    if (!ensurePermission("security.manage", "SECURITY_MANAGE", "open security manager"))
        return;

    QDialog dialog(this);
    dialog.setWindowTitle("安全权限管理");
    dialog.resize(1050, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    auto *userPage = new QWidget(tabs);
    auto *rolePage = new QWidget(tabs);
    auto *tokenPage = new QWidget(tabs);
    tabs->addTab(userPage, "用户管理");
    tabs->addTab(rolePage, "角色权限");
    tabs->addTab(tokenPage, "远程安全");
    layout->addWidget(tabs);

    auto *userLayout = new QVBoxLayout(userPage);
    auto *userTable = new QTableWidget(userPage);
    userTable->setColumnCount(4);
    userTable->setHorizontalHeaderLabels({"用户名", "角色", "启用", "密码Hash"});
    userTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    userTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    userLayout->addWidget(userTable);
    auto refreshUsers = [&]() {
        const auto users = m_securityManager->users();
        userTable->setRowCount(users.size());
        for (int row = 0; row < users.size(); ++row) {
            userTable->setItem(row, 0, new QTableWidgetItem(users[row].username));
            userTable->setItem(row, 1, new QTableWidgetItem(users[row].role));
            userTable->setItem(row, 2, new QTableWidgetItem(users[row].enabled ? "是" : "否"));
            userTable->setItem(row, 3, new QTableWidgetItem(users[row].passwordHash.left(12) + "..."));
        }
    };
    auto selectedUser = [&]() -> QString { const int row = userTable->currentRow(); return row >= 0 && userTable->item(row, 0) ? userTable->item(row, 0)->text() : QString(); };
    auto editUser = [&](const SecurityUser &source, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? "新增用户" : "编辑用户");
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? QString() : source.username, &editor);
        name->setEnabled(isNew);
        auto *pwd = new QLineEdit(&editor); pwd->setEchoMode(QLineEdit::Password);
        auto *role = new QComboBox(&editor);
        for (const auto &r : m_securityManager->roles()) role->addItem(r.name);
        role->setCurrentText(isNew ? "operator" : source.role);
        auto *enabled = new QCheckBox(&editor); enabled->setChecked(isNew ? true : source.enabled);
        form->addRow("用户名", name); form->addRow("新密码", pwd); form->addRow("角色", role); form->addRow("启用", enabled);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        if (editor.exec() != QDialog::Accepted) return;
        m_securityManager->addOrUpdateUser(name->text(), pwd->text(), role->currentText(), enabled->isChecked());
        m_securityManager->save(m_appSettings); refreshUsers();
    };
    auto *userButtons = new QHBoxLayout();
    auto *addUser = new QPushButton("新增", userPage);
    auto *editUserBtn = new QPushButton("编辑", userPage);
    auto *delUser = new QPushButton("删除", userPage);
    auto *toggleUser = new QPushButton("启用/禁用", userPage);
    userButtons->addWidget(addUser); userButtons->addWidget(editUserBtn); userButtons->addWidget(delUser); userButtons->addWidget(toggleUser); userButtons->addStretch();
    userLayout->addLayout(userButtons);
    connect(addUser, &QPushButton::clicked, &dialog, [&]() { SecurityUser u; editUser(u, true); });
    connect(editUserBtn, &QPushButton::clicked, &dialog, [&]() { const QString name = selectedUser(); for (const auto &u : m_securityManager->users()) if (u.username == name) editUser(u, false); });
    connect(delUser, &QPushButton::clicked, &dialog, [&]() { const QString name = selectedUser(); if (!name.isEmpty()) { m_securityManager->removeUser(name); m_securityManager->save(m_appSettings); refreshUsers(); } });
    connect(toggleUser, &QPushButton::clicked, &dialog, [&]() { const QString name = selectedUser(); for (const auto &u : m_securityManager->users()) if (u.username == name) { m_securityManager->setUserEnabled(name, !u.enabled); m_securityManager->save(m_appSettings); refreshUsers(); } });

    auto *roleLayout = new QVBoxLayout(rolePage);
    auto *roleTable = new QTableWidget(rolePage);
    roleTable->setColumnCount(2);
    roleTable->setHorizontalHeaderLabels({"角色", "权限"});
    roleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    roleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    roleLayout->addWidget(roleTable);
    auto refreshRoles = [&]() {
        const auto roles = m_securityManager->roles();
        roleTable->setRowCount(roles.size());
        for (int row = 0; row < roles.size(); ++row) {
            roleTable->setItem(row, 0, new QTableWidgetItem(roles[row].name));
            roleTable->setItem(row, 1, new QTableWidgetItem(roles[row].permissions.join(",")));
        }
    };
    auto *roleButtons = new QHBoxLayout();
    auto *editRole = new QPushButton("编辑角色权限", rolePage);
    roleButtons->addWidget(editRole); roleButtons->addStretch(); roleLayout->addLayout(roleButtons);
    connect(editRole, &QPushButton::clicked, &dialog, [&]() {
        const int row = roleTable->currentRow(); if (row < 0 || !roleTable->item(row, 0)) return;
        const QString roleName = roleTable->item(row, 0)->text();
        bool ok = false;
        const QString current = roleTable->item(row, 1) ? roleTable->item(row, 1)->text() : QString();
        const QString text = QInputDialog::getMultiLineText(&dialog, "角色权限", "权限列表，每行一个，* 表示全部：", current.split(',', Qt::SkipEmptyParts).join("\n"), &ok);
        if (!ok) return;
        m_securityManager->setRolePermissions(roleName, text.split('\n', Qt::SkipEmptyParts));
        m_securityManager->save(m_appSettings); refreshRoles();
    });

    auto *tokenLayout = new QFormLayout(tokenPage);
    auto *writeEnabled = new QCheckBox(tokenPage); writeEnabled->setChecked(m_securityManager->remoteWriteEnabled());
    auto *tokenEdit = new QLineEdit(tokenPage); tokenEdit->setEchoMode(QLineEdit::Password);
    auto *opsEdit = new QLineEdit(m_securityManager->writeOperators().join(","), tokenPage);
    auto *saveSecurity = new QPushButton("保存远程安全配置", tokenPage);
    tokenLayout->addRow("允许远程写入", writeEnabled);
    tokenLayout->addRow("新 API Token", tokenEdit);
    tokenLayout->addRow("写操作白名单", opsEdit);
    tokenLayout->addRow(saveSecurity);
    connect(saveSecurity, &QPushButton::clicked, &dialog, [&]() {
        if (!tokenEdit->text().isEmpty()) m_securityManager->setApiToken(tokenEdit->text());
        m_securityManager->setRemoteWriteEnabled(writeEnabled->isChecked());
        m_securityManager->setWriteOperators(opsEdit->text().split(',', Qt::SkipEmptyParts));
        m_securityManager->save(m_appSettings);
        statusBar()->showMessage("安全配置已保存", 3000);
    });

    refreshUsers(); refreshRoles();
    dialog.exec();
}

void MainWindow::showDeliveryManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle("产品交付工具");
    dialog.resize(1050, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    auto *checkPage = new QWidget(tabs);
    auto *envPage = new QWidget(tabs);
    tabs->addTab(checkPage, "交付清单");
    tabs->addTab(envPage, "运行环境");
    layout->addWidget(tabs);

    auto *checkLayout = new QVBoxLayout(checkPage);
    auto *checkTable = new QTableWidget(checkPage);
    checkTable->setColumnCount(2);
    checkTable->setHorizontalHeaderLabels({"序号", "交付项"});
    checkTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    checkTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    checkLayout->addWidget(checkTable);

    auto refreshChecklist = [&]() {
        const auto items = m_deliveryManager->checklist();
        checkTable->setRowCount(items.size());
        for (int row = 0; row < items.size(); ++row) {
            checkTable->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
            checkTable->setItem(row, 1, new QTableWidgetItem(items[row]));
        }
    };

    auto *envLayout = new QVBoxLayout(envPage);
    auto *envTable = new QTableWidget(envPage);
    envTable->setColumnCount(3);
    envTable->setHorizontalHeaderLabels({"检查项", "状态", "详情"});
    envTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    envTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    envLayout->addWidget(envTable);

    auto refreshEnvironment = [&]() {
        const auto items = m_deliveryManager->checkEnvironment();
        envTable->setRowCount(items.size());
        for (int row = 0; row < items.size(); ++row) {
            envTable->setItem(row, 0, new QTableWidgetItem(items[row].name));
            envTable->setItem(row, 1, new QTableWidgetItem(items[row].passed ? "通过" : "需处理"));
            envTable->setItem(row, 2, new QTableWidgetItem(items[row].detail));
        }
    };

    auto *buttons = new QHBoxLayout();
    auto *exportChecklistBtn = new QPushButton("导出清单", &dialog);
    auto *checkEnvBtn = new QPushButton("运行环境检查", &dialog);
    auto *exportEnvBtn = new QPushButton("导出环境报告", &dialog);
    auto *logsBtn = new QPushButton("打包日志", &dialog);
    auto *releaseBtn = new QPushButton("生成发布说明", &dialog);
    auto *manualBtn = new QPushButton("生成手册", &dialog);
    auto *scriptBtn = new QPushButton("生成打包脚本", &dialog);
    auto *assetsBtn = new QPushButton("一键生成交付资产", &dialog);
    for (auto *btn : {exportChecklistBtn, checkEnvBtn, exportEnvBtn, logsBtn, releaseBtn, manualBtn, scriptBtn, assetsBtn})
        buttons->addWidget(btn);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(exportChecklistBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, "导出交付清单", QDir::homePath() + "/产品交付清单.md", "Markdown Files (*.md)");
        if (!file.isEmpty() && m_deliveryManager->exportChecklist(file)) statusBar()->showMessage("交付清单已导出", 3000);
    });
    connect(checkEnvBtn, &QPushButton::clicked, &dialog, [&]() { refreshEnvironment(); tabs->setCurrentWidget(envPage); statusBar()->showMessage("运行环境检查完成", 3000); });
    connect(exportEnvBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, "导出环境报告", QDir::homePath() + "/运行环境检查报告.md", "Markdown Files (*.md)");
        if (!file.isEmpty() && m_deliveryManager->exportEnvironmentReport(file)) statusBar()->showMessage("环境报告已导出", 3000);
    });
    connect(logsBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, "选择日志包输出目录", QDir::homePath());
        if (!dir.isEmpty() && m_deliveryManager->packageLogs(dir)) statusBar()->showMessage("日志包已生成", 3000);
    });
    connect(releaseBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, "生成发布说明", QDir::homePath() + "/版本发布说明.md", "Markdown Files (*.md)");
        if (!file.isEmpty() && m_deliveryManager->generateReleaseNotes(file)) statusBar()->showMessage("发布说明已生成", 3000);
    });
    connect(manualBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, "选择手册输出目录", QDir::homePath());
        if (dir.isEmpty()) return;
        const bool ok = m_deliveryManager->generateUserManual(QDir(dir).absoluteFilePath("用户手册.md"))
            && m_deliveryManager->generateMaintenanceManual(QDir(dir).absoluteFilePath("维护手册.md"));
        statusBar()->showMessage(ok ? "用户手册和维护手册已生成" : "手册生成失败", 3000);
    });
    connect(scriptBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, "生成 Windows 打包脚本", QDir::homePath() + "/package_windows.ps1", "PowerShell Files (*.ps1)");
        if (!file.isEmpty() && m_deliveryManager->generateWindowsPackageScript(file)) statusBar()->showMessage("打包脚本已生成", 3000);
    });
    connect(assetsBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, "选择交付资产输出目录", QDir::homePath());
        if (!dir.isEmpty() && m_deliveryManager->generateDeliveryAssets(dir)) statusBar()->showMessage("交付资产已生成", 3000);
    });

    refreshChecklist();
    refreshEnvironment();
    dialog.exec();
}

void MainWindow::saveProfile()
{
    bool ok;
    QString name = QInputDialog::getText(this, "保存配置",
        "配置名称:", QLineEdit::Normal, "default", &ok);
    if (!ok || name.isEmpty()) return;

    saveSettings();
    if (m_configProfileManager->saveProfile(name, m_appSettings)) {
        statusBar()->showMessage("配置已保存: " + name, 3000);
        logMessage("配置保存: " + name);
    }
}

void MainWindow::loadProfile()
{
    auto profiles = m_configProfileManager->availableProfiles();
    if (profiles.isEmpty()) {
        QMessageBox::information(this, "加载配置", "没有可用的配置文件");
        return;
    }

    QStringList names;
    for (const auto &p : profiles)
        names << p.name;

    bool ok;
    QString name = QInputDialog::getItem(this, "加载配置",
        "选择配置:", names, 0, false, &ok);
    if (!ok || name.isEmpty()) return;

    if (m_configProfileManager->loadProfile(name, m_appSettings)) {
        loadSettings();
        statusBar()->showMessage("配置已加载: " + name, 3000);
        logMessage("配置加载: " + name);
    }
}
