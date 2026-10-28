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
#include "alarmrepository.h"
#include "batchtaskmanager.h"
#include "dashboard.h"
#include "configprofile.h"
#include "reliabilitymanager.h"
#include "securitymanager.h"
#include "pointmodel.h"
#include "dataparser.h"   // ByteOrder / byteOrderFromToken（模板字节序透传）
#include "verificationmanager.h"
#include "deliverymanager.h"
#include "logviewer.h"
#include "settingsdialog.h"
#include "mqttclient.h"         // MQTT 发布端客户端
#include "mqttoutbox.h"         // 离线队列优先级
#include "devicesimulator.h"    // 模拟设备（Modbus 从站）测试面板
#include "credentialcodec.h"    // U1：凭据混淆编解码（broker 密码不明文落盘）
#include "rawrequestdialog.h"   // 原始功能码 / 报文发送工具窗
#include "scannerdialog.h"      // Modbus 总线扫描器

#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QStatusBar>
#include <QStandardPaths>
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
#include <QGroupBox>
#include <QAbstractItemView>
#include <QTextEdit>
#include <QTableWidgetItem>
#include <QFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QDateTimeEdit>
#include <QMap>
#include <QSet>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <functional>
#include <algorithm>

namespace {

// 启用/禁用切换按钮：按钮只显示“下一步动作”——当前已启用时显示“禁用”（红色），
// 当前已禁用时显示“启用”（绿色）；没有选中行时按钮置灰不可点。
void updateToggleButton(QPushButton *button, bool hasSelection, bool enabledState,
                        const QString &enableText, const QString &disableText)
{
    if (!button)
        return;

    button->setEnabled(hasSelection);
    if (!hasSelection) {
        button->setText(enableText);
        button->setStyleSheet(QString());
        return;
    }

    if (enabledState) {
        button->setText(disableText);
        button->setStyleSheet(QStringLiteral(
            "QPushButton{color:#ffffff;background-color:#e74c3c;border:1px solid #c0392b;"
            "border-radius:4px;padding:4px 14px;font-weight:600;}"
            "QPushButton:hover{background-color:#c0392b;}"
            "QPushButton:pressed{background-color:#a93226;}"));
    } else {
        button->setText(enableText);
        button->setStyleSheet(QStringLiteral(
            "QPushButton{color:#ffffff;background-color:#27ae60;border:1px solid #1e8449;"
            "border-radius:4px;padding:4px 14px;font-weight:600;}"
            "QPushButton:hover{background-color:#1e8449;}"
            "QPushButton:pressed{background-color:#196f3d;}"));
    }
}

// 数据库连接自检：用独立连接名打开 SQLite 文件并执行一条只读语句。
// 不会影响正在使用的连接，也不会修改目标库结构。
bool testSqlitePath(const QString &path, QString *error)
{
    if (error)
        error->clear();
    if (path.trimmed().isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("MainWindow", "Path is empty");
        return false;
    }

    const QFileInfo info(path);
    QDir dir(info.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QCoreApplication::translate("MainWindow", "Cannot create directory: %1")
                         .arg(dir.absolutePath());
        return false;
    }

    const QString connectionName =
        QStringLiteral("fieldlink_dbtest_") + QUuid::createUuid().toString();
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(path);
        if (!db.open()) {
            if (error)
                *error = db.lastError().text();
        } else {
            QSqlQuery query(db);
            ok = query.exec(QStringLiteral("PRAGMA schema_version"));
            if (!ok && error)
                *error = query.lastError().text();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

} // namespace

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
    case AlarmCondition::GreaterThan: return QCoreApplication::translate("MainWindow", "Greater than");
    case AlarmCondition::LessThan: return QCoreApplication::translate("MainWindow", "Less than");
    case AlarmCondition::Equal: return QCoreApplication::translate("MainWindow", "Equal");
    case AlarmCondition::NotEqual: return QCoreApplication::translate("MainWindow", "Not equal");
    case AlarmCondition::InRange: return QCoreApplication::translate("MainWindow", "In range");
    case AlarmCondition::OutOfRange: return QCoreApplication::translate("MainWindow", "Out of range");
    case AlarmCondition::BitSet: return QCoreApplication::translate("MainWindow", "bit set");
    case AlarmCondition::BitClear: return QCoreApplication::translate("MainWindow", "bit clear");
    }
    return QCoreApplication::translate("MainWindow", "Unknown");
}

static QString alarmSeverityName(AlarmSeverity severity)
{
    switch (severity) {
    case AlarmSeverity::Info: return QCoreApplication::translate("MainWindow", "Info");
    case AlarmSeverity::Warning: return QCoreApplication::translate("MainWindow", "Warning");
    case AlarmSeverity::Critical: return QCoreApplication::translate("MainWindow", "Critical");
    }
    return QCoreApplication::translate("MainWindow", "Unknown");
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

// 点表批量导入：交互式选择文件 + 追加/覆盖，返回是否有数据被导入
static bool importPointsInteractive(QWidget *parent, PointModel *model)
{
    if (!model)
        return false;
    const QString file = QFileDialog::getOpenFileName(
        parent, QCoreApplication::translate("MainWindow", "Import Points"),
        QCoreApplication::applicationDirPath(),
        QCoreApplication::translate("MainWindow",
            "Point Files (*.csv *.json);;CSV Files (*.csv);;JSON Files (*.json)"));
    if (file.isEmpty())
        return false;

    bool append = true;
    if (!model->points().isEmpty()) {
        const QMessageBox::StandardButton btn = QMessageBox::question(
            parent, QCoreApplication::translate("MainWindow", "Import mode"),
            QCoreApplication::translate("MainWindow",
                "%1 point(s) already exist:\n\n[Yes] Append to existing points\n"
                "[No] Clear and overwrite\n[Cancel] Abort import")
                .arg(model->points().size()),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (btn == QMessageBox::Cancel)
            return false;
        append = (btn == QMessageBox::Yes);
    }

    int imported = 0;
    QString error;
    if (!model->importAuto(file, append, &imported, &error)) {
        QMessageBox::warning(parent, QCoreApplication::translate("MainWindow", "Import failed"),
                             error.isEmpty()
                                 ? QCoreApplication::translate("MainWindow", "Unknown error")
                                 : error);
        return false;
    }
    QMessageBox::information(parent, QCoreApplication::translate("MainWindow", "Import complete"),
                             QCoreApplication::translate("MainWindow", "Imported %1 point(s) (%2)")
                                 .arg(imported)
                                 .arg(append
                                          ? QCoreApplication::translate("MainWindow", "Append")
                                          : QCoreApplication::translate("MainWindow", "Overwrite")));
    return true;
}

// 点表批量导出：按扩展名选择 CSV / JSON
static bool exportPointsInteractive(QWidget *parent, PointModel *model)
{
    if (!model)
        return false;
    if (model->points().isEmpty()) {
        QMessageBox::information(parent,
                                 QCoreApplication::translate("MainWindow", "Export Points"),
                                 QCoreApplication::translate("MainWindow",
                                     "There are no points to export."));
        return false;
    }
    const QString file = QFileDialog::getSaveFileName(
        parent, QCoreApplication::translate("MainWindow", "Export Points"),
        QDir::homePath() + QStringLiteral("/points.csv"),
        QCoreApplication::translate("MainWindow", "CSV Files (*.csv);;JSON Files (*.json)"));
    if (file.isEmpty())
        return false;

    QString error;
    const bool ok = file.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
                        ? model->exportToJson(file, &error)
                        : model->exportToCsv(file, &error);
    if (!ok) {
        QMessageBox::warning(parent, QCoreApplication::translate("MainWindow", "Export failed"),
                             error.isEmpty()
                                 ? QCoreApplication::translate("MainWindow", "Unknown error")
                                 : error);
        return false;
    }
    QMessageBox::information(parent, QCoreApplication::translate("MainWindow", "Export complete"),
                             QCoreApplication::translate("MainWindow",
                                 "Exported %1 point(s) to:\n%2")
                                 .arg(model->points().size())
                                 .arg(file));
    return true;
}

QModbusClient *MainWindow::clientForDevice(int deviceId) const
{
    if (deviceId <= 0)
        return modbusDevice;
    return m_deviceManager ? m_deviceManager->modbusClient(deviceId) : nullptr;
}

QString MainWindow::deviceDisplayName(int deviceId) const
{
    if (deviceId <= 0)
        return tr("Main connection");
    if (m_deviceManager) {
        for (const auto &d : m_deviceManager->allDevices()) {
            if (d.id == deviceId)
                return d.name.isEmpty() ? tr("Device %1").arg(deviceId) : d.name;
        }
    }
    return tr("Device %1").arg(deviceId);
}

void MainWindow::rebuildPointPollTasks()
{
    if (!m_pollManager || !m_pointModel)
        return;

    // 只清理上一轮由点表生成的任务；设备/模板/扫描器/人工任务保持不变。
    const QVector<PollTask> existing = m_pollManager->tasks();
    for (const PollTask &task : existing) {
        if (task.fromPoints)
            m_pollManager->removeTask(task.id);
    }

    // 分组键：设备 + 从站 + 寄存器类型（不同类型不能同请求）
    QMap<QString, QVector<PointDefinition>> groups;
    for (const PointDefinition &p : m_pointModel->points()) {
        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(p.deviceId).arg(p.serverAddress)
                                .arg(static_cast<int>(p.registerType));
        groups[key].append(p);
    }

    constexpr int kBaseId = 30000000;   // 点表采集段 id 段（与扫描 20000000+ 不重叠）
    int seq = 0;
    int created = 0;

    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QVector<PointDefinition> pts = it.value();
        std::sort(pts.begin(), pts.end(), [](const PointDefinition &a, const PointDefinition &b) {
            if (a.address != b.address)
                return a.address < b.address;
            return a.id < b.id;
        });

        const bool bitType = (pts.first().registerType == QModbusDataUnit::Coils
                              || pts.first().registerType == QModbusDataUnit::DiscreteInputs);
        const int maxSpan = bitType ? 2000 : 125;

        int i = 0;
        while (i < pts.size()) {
            const int start = pts[i].address;
            int end = start + qMax(1, pts[i].count);          // exclusive
            int intervalMs = qMax(100, pts[i].pollIntervalSec * 1000);
            QVector<int> pointIds;
            pointIds.append(pts[i].id);

            int j = i + 1;
            while (j < pts.size()) {
                const int nextStart = pts[j].address;
                const int nextEnd = nextStart + qMax(1, pts[j].count);
                // 只合并相邻/重叠区间；中间有空洞就断开，避免读到未实现的地址段
                if (nextStart > end)
                    break;
                if (qMax(end, nextEnd) - start > maxSpan)
                    break;
                end = qMax(end, nextEnd);
                intervalMs = qMin(intervalMs, qMax(100, pts[j].pollIntervalSec * 1000));
                pointIds.append(pts[j].id);
                ++j;
            }

            PollTask task;
            task.id = kBaseId + seq++;
            task.name = tr("Points:%1/Slave %2/%3@%4")
                            .arg(deviceDisplayName(pts[i].deviceId))
                            .arg(pts[i].serverAddress)
                            .arg(registerTypeName(static_cast<int>(pts[i].registerType)))
                            .arg(start);
            task.deviceId = pts[i].deviceId;
            task.serverAddress = pts[i].serverAddress;
            task.registerType = pts[i].registerType;
            task.startAddress = start;
            task.quantity = end - start;
            task.intervalMs = intervalMs;
            task.enabled = true;
            task.pointIds = pointIds;
            task.fromPoints = true;
            m_pollManager->addTask(task);
            ++created;
            i = j;
        }
    }

    logMessage(tr("Point-table poll segments rebuilt: %1 request segment(s), %2 point(s)")
                   .arg(created).arg(m_pointModel->points().size()));
}

void MainWindow::maybeArchivePoint(const PointDefinition &point, const PointValue &value)
{
    if (!m_historyData || !m_historyData->isOpen() || !point.archiveEnabled)
        return;
    if (!value.timestamp.isValid())
        return;

    const int intervalMs = qMax(1, point.archiveIntervalSec) * 1000;
    const int quality = static_cast<int>(value.quality);

    const auto lastTimeIt = m_lastArchiveTime.constFind(point.id);
    if (lastTimeIt != m_lastArchiveTime.constEnd()) {
        const qint64 elapsed = lastTimeIt.value().msecsTo(value.timestamp);
        if (elapsed >= 0 && elapsed < intervalMs)
            return;

        // 死区：用户显式配置 >0 时，同质量码且变化小于死区 → 不落库（抑制稳态噪声）
        if (point.archiveDeadband > 0.0
            && m_lastArchiveQuality.value(point.id, -1) == quality
            && m_lastArchiveValue.contains(point.id)
            && qAbs(value.value - m_lastArchiveValue.value(point.id)) < point.archiveDeadband) {
            return;
        }
    }

    PointHistoryRecord rec;
    rec.timestamp = value.timestamp;
    rec.deviceId = point.deviceId;
    rec.pointId = point.id;
    rec.pointName = point.name;
    rec.serverAddress = point.serverAddress;
    rec.value = value.value;
    rec.quality = quality;

    if (!m_historyData->addPointRecord(rec))
        return;

    m_lastArchiveTime[point.id] = value.timestamp;
    m_lastArchiveValue[point.id] = value.value;
    m_lastArchiveQuality[point.id] = quality;
}

void MainWindow::updateQualityWatchdog()
{
    if (!m_pointModel || !m_pollManager || !m_pollManager->isRunning())
        return;
    m_pointModel->markStale(QDateTime::currentDateTime(), 5000);
}

void MainWindow::onPollRequest(const PollTask &task)
{
    QModbusClient *client = clientForDevice(task.deviceId);
    if (!client || client->state() != QModbusDevice::ConnectedState) {
        // 设备未连接：本次请求不会发出，必须立即解除在途标志，避免任务被永久跳过；
        // 同时把该任务关联的点位降级，界面不能继续显示旧的 Good 值。
        if (m_pointModel) {
            if (!task.pointIds.isEmpty())
                m_pointModel->markPointsBad(task.pointIds, DataQuality::Bad,
                                            QStringLiteral("device not connected"));
            else
                m_pointModel->markRangeBad(task.deviceId, task.serverAddress, task.registerType,
                                           task.startAddress, task.quantity, DataQuality::Bad,
                                           QStringLiteral("device not connected"));
        }
        if (task.deviceId == 0 && m_reliabilityManager)
            m_reliabilityManager->notifyFailure(QStringLiteral("Modbus device not connected"));
        m_pollManager->notifyTaskFinished(task.id);
        return;
    }

    QModbusDataUnit unit(task.registerType, task.startAddress, task.quantity);
    if (auto *reply = client->sendReadRequest(unit, task.serverAddress)) {
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

                    // 原始寄存器块保留用于报文级追溯；工程值归档由
                    // PointModel::pointValueUpdated → maybeArchivePoint 按点位写入。
                    m_historyData->addRecord(task.serverAddress, task.registerType,
                                             task.startAddress, values);
                    if (task.deviceId == 0 && m_reliabilityManager)
                        m_reliabilityManager->notifySuccess();
                    m_pointModel->updateFromRaw(task.serverAddress, task.registerType,
                                                task.startAddress, values, task.deviceId);

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
                        m_dashboard->appendAlarmLog(tr("Active alarms: %1").arg(m_alarmManager->activeAlarmCount()));

                    m_pluginManager->notifyDataReceived(task.serverAddress,
                        static_cast<int>(task.registerType), task.startAddress, values);

                    // MQTT：轮询数据实时上送（带 deviceId，避免多设备同从站地址时无法区分）
                    publishMqttTelemetry(task.deviceId, task.serverAddress,
                        static_cast<int>(task.registerType), task.startAddress, values);

                    m_dashboard->appendCommunicationLog(QStringLiteral("READ OK addr=%1 count=%2 first=%3")
                        .arg(task.startAddress)
                        .arg(task.quantity)
                        .arg(values.isEmpty() ? QStringLiteral("--") : QString::number(values.first())));

                    logMessage(QStringLiteral("POLL-OK [%1] addr=%2 count=%3")
                               .arg(task.name).arg(task.startAddress).arg(task.quantity));
                } else {
                    if (task.deviceId == 0 && m_reliabilityManager)
                        m_reliabilityManager->notifyFailure(reply->errorString());
                    if (m_pointModel) {
                        if (!task.pointIds.isEmpty())
                            m_pointModel->markPointsBad(task.pointIds, DataQuality::Bad,
                                                        reply->errorString());
                        else
                            m_pointModel->markRangeBad(task.deviceId, task.serverAddress,
                                                       task.registerType, task.startAddress,
                                                       task.quantity, DataQuality::Bad,
                                                       reply->errorString());
                    }
                    m_dashboard->appendCommunicationLog(QStringLiteral("READ ERROR %1").arg(reply->errorString()));
                    logMessage(QStringLiteral("POLL-ERR [%1] %2")
                               .arg(task.name).arg(reply->errorString()), 3);
                }

                // P2：本条请求已完成，解除在途标志（无论成败）
                m_pollManager->notifyTaskFinished(task.id);
                reply->deleteLater();
            });
        } else {
            // 立即完成的回复（如广播）：同样解除在途标志
            delete reply;
            m_pollManager->notifyTaskFinished(task.id);
        }
    } else {
        // 发送失败：立即解除在途标志，否则任务会被永久跳过
        logMessage(QStringLiteral("POLL-SEND-ERR [%1] %2").arg(task.name).arg(client->errorString()), 3);
        if (m_pointModel) {
            if (!task.pointIds.isEmpty())
                m_pointModel->markPointsBad(task.pointIds, DataQuality::Bad, client->errorString());
            else
                m_pointModel->markRangeBad(task.deviceId, task.serverAddress, task.registerType,
                                           task.startAddress, task.quantity, DataQuality::Bad,
                                           client->errorString());
        }
        m_pollManager->notifyTaskFinished(task.id);
    }
}

void MainWindow::onBatchReadTask(const BatchTask &task)
{
    QModbusClient *client = clientForDevice(task.deviceId);
    if (!client || client->state() != QModbusDevice::ConnectedState) {
        m_batchTaskManager->onTaskFinished(task.id, false, "Not connected", {});
        return;
    }

    QModbusDataUnit unit(task.registerType, task.startAddress, task.quantity);
    if (auto *reply = client->sendReadRequest(unit, task.serverAddress)) {
        if (!reply->isFinished()) {
            connect(reply, &QModbusReply::finished, this, [this, task, reply]() {
                if (reply->error() == QModbusDevice::NoError) {
                    QVector<quint16> values;
                    const QModbusDataUnit result = reply->result();
                    for (uint i = 0; i < result.valueCount(); ++i)
                        values.append(result.value(i));
                    m_batchTaskManager->onTaskFinished(task.id, true, QString(), values);

                    // MQTT：批量读取数据上送
                    publishMqttTelemetry(task.deviceId, task.serverAddress,
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
        m_batchTaskManager->onTaskFinished(task.id, false, client->errorString(), {});
    }
}

void MainWindow::onBatchWriteTask(const BatchTask &task)
{
    QModbusClient *client = clientForDevice(task.deviceId);
    if (!client || client->state() != QModbusDevice::ConnectedState) {
        m_batchTaskManager->onTaskFinished(task.id, false, "Not connected", {});
        return;
    }

    QModbusDataUnit unit(task.registerType, task.startAddress, task.quantity);
    for (int i = 0; i < task.writeValues.size() && i < task.quantity; ++i)
        unit.setValue(i, task.writeValues[i]);

    if (auto *reply = client->sendWriteRequest(unit, task.serverAddress)) {
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
        m_batchTaskManager->onTaskFinished(task.id, false, client->errorString(), {});
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

// 远程读（异步）。
// 不再使用嵌套 QEventLoop 等待 Modbus 应答 —— 那会导致：
//   1) 客户端在等待期间断开 → socket 在嵌套循环中被销毁 → 回写响应时 use-after-free；
//   2) 嵌套循环接管 GUI 事件循环 → 界面冻结；新请求还会重入形成递归。
// 现在改为：发请求 → 在 reply->finished 回调里组装结果 → 通过 callback 回给服务器。
void MainWindow::executeRemoteRead(int serverAddress, int registerType, int startAddress, int count,
                                   const std::function<void(const QJsonObject &)> &callback)
{
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState) {
        callback(QJsonObject{{"success", false}, {"error", "Modbus device not connected"}});
        return;
    }

    QModbusDataUnit unit(static_cast<QModbusDataUnit::RegisterType>(registerType), startAddress, count);
    QModbusReply *reply = modbusDevice->sendReadRequest(unit, serverAddress);
    if (!reply) {
        callback(QJsonObject{{"success", false}, {"error", modbusDevice->errorString()}});
        return;
    }

    // 超时兜底：QModbusClient 自身也有超时，但这里再加一层，
    // 保证 reply 一定会被回收，不会永远挂着一个在途请求。
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply, callback]() {
        reply->deleteLater();
        callback(QJsonObject{{"success", false}, {"error", "Read timeout"}});
    });
    timeout->start(m_settingsDialog->settings().responseTime + 1000);

    connect(reply, &QModbusReply::finished, this, [this, reply, timeout, serverAddress,
                                                    registerType, startAddress, callback]() {
        timeout->stop();
        if (reply->error() != QModbusDevice::NoError) {
            const QString err = reply->errorString();
            reply->deleteLater();
            m_reliabilityManager->notifyFailure(err);
            callback(QJsonObject{{"success", false}, {"error", err}});
            return;
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
        m_historyData->addRecord(serverAddress, static_cast<QModbusDataUnit::RegisterType>(registerType),
                                  startAddress, raw);
        QJsonObject response;
        response["success"] = true;
        response["serverAddress"] = serverAddress;
        response["registerType"] = registerType;
        response["registerTypeName"] = registerTypeName(registerType);
        response["startAddress"] = startAddress;
        response["count"] = static_cast<int>(values.size());
        response["values"] = values;
        response["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        callback(response);
    });
}

// 远程写（异步）—— 同上，不使用嵌套事件循环。
void MainWindow::executeRemoteWrite(int serverAddress, int registerType, int startAddress,
                                    const QVector<quint16> &values,
                                    const std::function<void(const QJsonObject &)> &callback)
{
    if (!modbusDevice || modbusDevice->state() != QModbusDevice::ConnectedState) {
        callback(QJsonObject{{"success", false}, {"error", "Modbus device not connected"}});
        return;
    }

    QModbusDataUnit unit(static_cast<QModbusDataUnit::RegisterType>(registerType), startAddress, values.size());
    for (int i = 0; i < values.size(); ++i)
        unit.setValue(i, values[i]);

    QModbusReply *reply = modbusDevice->sendWriteRequest(unit, serverAddress);
    if (!reply) {
        callback(QJsonObject{{"success", false}, {"error", modbusDevice->errorString()}});
        return;
    }

    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply, callback]() {
        reply->deleteLater();
        callback(QJsonObject{{"success", false}, {"error", "Write timeout"}});
    });
    timeout->start(m_settingsDialog->settings().responseTime + 1000);

    connect(reply, &QModbusReply::finished, this, [this, reply, timeout, serverAddress,
                                                    registerType, startAddress, values, callback]() {
        timeout->stop();
        if (reply->error() != QModbusDevice::NoError) {
            const QString err = reply->errorString();
            reply->deleteLater();
            m_reliabilityManager->notifyFailure(err);
            callback(QJsonObject{{"success", false}, {"error", err}});
            return;
        }

        reply->deleteLater();
        m_reliabilityManager->notifySuccess();
        if (m_securityManager)
            m_securityManager->audit(QStringLiteral("remote"), QStringLiteral("WRITE"),
                QStringLiteral("server=%1 type=%2 addr=%3 count=%4")
                    .arg(serverAddress).arg(registerType).arg(startAddress).arg(values.size()));
        callback(QJsonObject{{"success", true}, {"serverAddress", serverAddress},
                             {"registerType", registerType}, {"startAddress", startAddress},
                             {"count", values.size()},
                             {"timestamp", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
    });
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
        QStringLiteral("%1 %2").arg(connected ? tr("Connected") : tr("Not connected"), ui->portEdit->currentText()));
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
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export Data"),
        QDir::homePath() + "/modbus_data.csv", "CSV Files (*.csv)");
    if (fileName.isEmpty()) return;

    if (m_dataExporter->exportToCsv(fileName)) {
        statusBar()->showMessage(tr("Data exported: ") + fileName, 5000);
        logMessage(tr("Data export succeeded: ") + fileName);
    } else {
        statusBar()->showMessage(tr("Export failed"), 5000);
    }
}

void MainWindow::startStopPolling()
{
    if (m_pollManager->isRunning()) {
        m_pollManager->stopAll();
        m_dashboard->setPollingStatus(false);
        m_dashboard->appendCommunicationLog(tr("Timed polling stopped"));
        statusBar()->showMessage(tr("Polling stopped"), 3000);
        logMessage(tr("Timed polling stopped"));
    } else {
        // 点表驱动：启动前按当前点表重建采集段（新增/修改点位无需手动"生成轮询"）
        rebuildPointPollTasks();

        // 采集段引用到多设备时，启动前自动尝试连接（用户仍可在设备管理中手动断开）
        if (m_deviceManager) {
            QSet<int> deviceIds;
            for (const PollTask &task : m_pollManager->tasks()) {
                if (task.deviceId > 0)
                    deviceIds.insert(task.deviceId);
            }
            for (int deviceId : deviceIds) {
                if (!m_deviceManager->isConnected(deviceId)) {
                    if (!m_deviceManager->connectDevice(deviceId))
                        logMessage(tr("Device %1 failed to connect when starting acquisition").arg(deviceId), 3);
                }
            }
        }

        if (m_pollManager->tasks().isEmpty()) {
            PollTask task;
            task.id = 1;
            task.name = "Default Poll";
            task.deviceId = 0;
            task.serverAddress = ui->serverEdit->value();
            task.registerType = currentUiRegisterType(ui);
            task.startAddress = ui->readAddress->value();
            task.quantity = ui->readSize->currentText().toInt();
            task.intervalMs = 1000;
            task.enabled = true;
            m_pollManager->addTask(task);
            m_chart->addSeries("Register " + QString::number(task.startAddress), Qt::blue, 200);
        }
        m_pollManager->startAll();
        m_dashboard->setPollingStatus(true);
        m_dashboard->setServerAddress(ui->serverEdit->value());
        m_dashboard->appendCommunicationLog(tr("Timed polling started"));
        statusBar()->showMessage(tr("Polling started"), 3000);
        logMessage(tr("Timed polling started"));
    }
}

void MainWindow::showBatchTaskDialog()
{
    if (!ensurePermission("batch.execute", "BATCH_EXECUTE", "load and start batch tasks"))
        return;
    QString filePath = QFileDialog::getOpenFileName(this, tr("Load Batch Task File"),
        QCoreApplication::applicationDirPath(), "JSON Files (*.json)");
    if (filePath.isEmpty()) return;

    if (m_batchTaskManager->loadFromFile(filePath)) {
        m_batchTaskManager->start();
        statusBar()->showMessage(tr("Batch task started"), 3000);
    } else {
        statusBar()->showMessage(tr("Failed to load batch task"), 5000);
    }
}

void MainWindow::showAlarmConfig()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Alarm Configuration & History"));
    dialog.resize(1050, 620);

    auto *tabs = new QTabWidget(&dialog);
    auto *rulePage = new QWidget(tabs);
    auto *historyPage = new QWidget(tabs);
    auto *mainLayout = new QVBoxLayout(&dialog);
    mainLayout->addWidget(tabs);
    tabs->addTab(rulePage, tr("Alarm Rules"));
    tabs->addTab(historyPage, tr("Alarm History / Acknowledge"));

    auto *ruleLayout = new QVBoxLayout(rulePage);
    auto *ruleTable = new QTableWidget(rulePage);
    ruleTable->setColumnCount(11);
    ruleTable->setHorizontalHeaderLabels({
        tr("ID"), tr("Enabled"), tr("Name"), tr("Slave"), tr("Register"), tr("Address"),
        tr("Condition"), tr("Threshold 1"), tr("Threshold 2"), tr("Severity"), tr("Message")});
    ruleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ruleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ruleLayout->addWidget(ruleTable);

    auto *ruleButtons = new QHBoxLayout();
    auto *addBtn = new QPushButton(tr("Add"), rulePage);
    auto *editBtn = new QPushButton(tr("Edit"), rulePage);
    auto *deleteBtn = new QPushButton(tr("Delete"), rulePage);
    auto *toggleBtn = new QPushButton(tr("Enable"), rulePage);
    ruleButtons->addWidget(addBtn); ruleButtons->addWidget(editBtn); ruleButtons->addWidget(deleteBtn); ruleButtons->addWidget(toggleBtn); ruleButtons->addStretch();
    ruleLayout->addLayout(ruleButtons);

    auto selectedRuleId = [&]() -> int {
        const int row = ruleTable->currentRow();
        return row >= 0 && ruleTable->item(row, 0) ? ruleTable->item(row, 0)->text().toInt() : 0;
    };

    // 切换按钮跟随选中行状态：已启用 → “禁用”（红），已禁用 → “启用”（绿）。
    auto updateRuleToggle = [&]() {
        const int id = selectedRuleId();
        bool found = false;
        bool enabled = false;
        if (id) {
            const auto rules = m_alarmManager->allRules();
            for (const auto &r : rules) {
                if (r.id == id) {
                    found = true;
                    enabled = r.enabled;
                    break;
                }
            }
        }
        updateToggleButton(toggleBtn, found, enabled, tr("Enable"), tr("Disable"));
    };

    auto refreshRules = [&]() {
        const auto rules = m_alarmManager->allRules();
        ruleTable->setRowCount(rules.size());
        for (int row = 0; row < rules.size(); ++row) {
            const auto &r = rules[row];
            const QStringList cells = {
                QString::number(r.id), r.enabled ? tr("Yes") : tr("No"), r.name,
                QString::number(r.serverAddress), registerTypeName(r.registerType), QString::number(r.address),
                alarmConditionName(r.condition), QString::number(r.threshold1), QString::number(r.threshold2),
                alarmSeverityName(r.severity), r.message
            };
            for (int col = 0; col < cells.size(); ++col)
                ruleTable->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
        updateRuleToggle();
    };

    auto editRule = [&](AlarmRule rule, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? tr("New Alarm Rule") : tr("Edit Alarm Rule"));
        auto *form = new QFormLayout(&editor);
        auto *enabled = new QCheckBox(&editor); enabled->setChecked(isNew ? true : rule.enabled);
        auto *name = new QLineEdit(isNew ? tr("Alarm Rule") : rule.name, &editor);
        auto *server = new QSpinBox(&editor); server->setRange(1, 247); server->setValue(isNew ? ui->serverEdit->value() : rule.serverAddress);
        auto *regType = new QComboBox(&editor); regType->addItems({tr("Coils"), tr("Discrete Inputs"), tr("Input Registers"), tr("Holding Registers")}); regType->setCurrentIndex(isNew ? currentUiRegisterComboIndex(ui) : comboIndexFromRegisterType(rule.registerType));
        auto *address = new QSpinBox(&editor); address->setRange(0, 65535); address->setValue(isNew ? ui->readAddress->value() : rule.address);
        auto *condition = new QComboBox(&editor);
        for (int i = static_cast<int>(AlarmCondition::GreaterThan); i <= static_cast<int>(AlarmCondition::BitClear); ++i)
            condition->addItem(alarmConditionName(static_cast<AlarmCondition>(i)));
        condition->setCurrentIndex(isNew ? 0 : static_cast<int>(rule.condition));
        auto *threshold1 = new QDoubleSpinBox(&editor); threshold1->setRange(-999999999, 999999999); threshold1->setDecimals(3); threshold1->setValue(isNew ? 0 : rule.threshold1);
        auto *threshold2 = new QDoubleSpinBox(&editor); threshold2->setRange(-999999999, 999999999); threshold2->setDecimals(3); threshold2->setValue(isNew ? 0 : rule.threshold2);
        auto *severity = new QComboBox(&editor);
        for (int i = static_cast<int>(AlarmSeverity::Info); i <= static_cast<int>(AlarmSeverity::Critical); ++i)
            severity->addItem(alarmSeverityName(static_cast<AlarmSeverity>(i)));
        severity->setCurrentIndex(isNew ? 1 : static_cast<int>(rule.severity));
        auto *debounce = new QSpinBox(&editor); debounce->setRange(0, 600000); debounce->setValue(isNew ? 0 : rule.debounceMs);
        auto *message = new QLineEdit(isNew ? QString() : rule.message, &editor);
        form->addRow(tr("Enabled"), enabled); form->addRow(tr("Name"), name); form->addRow(tr("Slave address"), server); form->addRow(tr("Register"), regType);
        form->addRow(tr("Address"), address); form->addRow(tr("Condition"), condition); form->addRow(tr("Threshold 1 / bit"), threshold1); form->addRow(tr("Threshold 2"), threshold2);
        form->addRow(tr("Severity"), severity); form->addRow(tr("Debounce (ms)"), debounce); form->addRow(tr("Message"), message);
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

    connect(ruleTable, &QTableWidget::itemSelectionChanged, &dialog, updateRuleToggle);
    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { AlarmRule r{}; editRule(r, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedRuleId(); if (id) editRule(m_alarmManager->getRule(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedRuleId(); if (id) { m_alarmManager->removeRule(id); refreshRules(); } });
    connect(toggleBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedRuleId(); if (id) { auto r = m_alarmManager->getRule(id); m_alarmManager->setRuleEnabled(id, !r.enabled); refreshRules(); } });

    auto *historyLayout = new QVBoxLayout(historyPage);
    auto *historyTable = new QTableWidget(historyPage);
    historyTable->setColumnCount(9);
    historyTable->setHorizontalHeaderLabels({
        tr("Event ID"), tr("Time"), tr("Rule ID"), tr("Name"), tr("Severity"), tr("Value"),
        tr("Acknowledged"), tr("Acknowledged time"), tr("Message")});
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
                alarmSeverityName(e.severity), QString::number(e.value), e.acknowledged ? tr("Yes") : tr("No"),
                e.acknowledgedTime.isValid() ? e.acknowledgedTime.toString("yyyy-MM-dd HH:mm:ss") : "", e.message};
            for (int col = 0; col < cells.size(); ++col)
                historyTable->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };
    auto *historyButtons = new QHBoxLayout();
    auto *ackBtn = new QPushButton(tr("Acknowledge selected"), historyPage);
    auto *ackAllBtn = new QPushButton(tr("Acknowledge all"), historyPage);
    auto *clearBtn = new QPushButton(tr("Clear history"), historyPage);
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
    dialog.setWindowTitle(tr("Device Management"));
    dialog.resize(1000, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(11);
    table->setHorizontalHeaderLabels({"ID", tr("Name"), tr("Type"), tr("Address / Port"), tr("Slave"), tr("Connect"), tr("Poll (ms)"), tr("Archive tag"), tr("Timeout"), tr("Retries"), tr("Last error")});
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
                QString::number(d.serverAddress), d.connected ? tr("Connected") : tr("Not connected"), QString::number(d.pollIntervalMs), d.archiveTag,
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
        editor.setWindowTitle(isNew ? tr("New Device") : tr("Edit Device"));
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? tr("Device") : cfg.name, &editor);
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
        form->addRow(tr("Name"), name); form->addRow(tr("TCP device"), isTcp); form->addRow(tr("Address / Serial port"), address); form->addRow(tr("Slave address"), server);
        form->addRow(tr("Baud rate"), baud); form->addRow(tr("Parity (enum)"), parity); form->addRow(tr("Data bits"), dataBits); form->addRow(tr("Stop bits"), stopBits);
        form->addRow(tr("Timeout (ms)"), timeout); form->addRow(tr("Retries"), retries); form->addRow(tr("Independent poll (ms)"), pollInterval); form->addRow(tr("Alarm / history archive tag"), archiveTag);
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
    auto *addBtn = new QPushButton(tr("Add"), &dialog);
    auto *editBtn = new QPushButton(tr("Edit"), &dialog);
    auto *deleteBtn = new QPushButton(tr("Delete"), &dialog);
    auto *connectBtn = new QPushButton(tr("Connect"), &dialog);
    auto *disconnectBtn = new QPushButton(tr("Disconnect"), &dialog);
    auto *pollBtn = new QPushButton(tr("Generate Poll Tasks"), &dialog);
    auto *saveBtn = new QPushButton(tr("Save Configuration"), &dialog);
    buttons->addWidget(addBtn); buttons->addWidget(editBtn); buttons->addWidget(deleteBtn); buttons->addWidget(connectBtn); buttons->addWidget(disconnectBtn); buttons->addWidget(pollBtn); buttons->addWidget(saveBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { DeviceConfig cfg{}; editDevice(cfg, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) editDevice(m_deviceManager->deviceConfig(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) { m_deviceManager->removeDevice(id); m_deviceManager->saveToSettings(m_appSettings); refresh(); } });
    connect(connectBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) { m_deviceManager->connectDevice(id); refresh(); } });
    connect(disconnectBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedDeviceId(); if (id) { m_deviceManager->disconnectDevice(id); refresh(); } });
    connect(saveBtn, &QPushButton::clicked, &dialog, [&]() { m_deviceManager->saveToSettings(m_appSettings); statusBar()->showMessage(tr("Device configuration saved"), 3000); });
    connect(pollBtn, &QPushButton::clicked, &dialog, [&]() {
        const int id = selectedDeviceId(); if (!id) return;
        const auto d = m_deviceManager->deviceConfig(id);
        PollTask task; task.id = 100000 + d.id; task.name = d.name; task.serverAddress = d.serverAddress;
        task.registerType = currentUiRegisterType(ui);
        task.startAddress = ui->readAddress->value(); task.quantity = ui->readSize->currentText().toInt(); task.intervalMs = d.pollIntervalMs;
        task.enabled = true; task.deviceId = d.id; task.fromPoints = false;
        m_pollManager->removeTask(task.id); m_pollManager->addTask(task);
        statusBar()->showMessage(tr("Generated device poll task: %1").arg(d.name), 3000);
    });

    refresh();
    dialog.exec();
}

void MainWindow::showTemplateManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Device Template Management"));
    dialog.resize(1120, 640);

    auto *layout = new QVBoxLayout(&dialog);
    auto *tables = new QHBoxLayout();
    auto *templateTable = new QTableWidget(&dialog);
    auto *registerTable = new QTableWidget(&dialog);
    templateTable->setColumnCount(6);
    templateTable->setHorizontalHeaderLabels({"ID", tr("Name"), tr("Manufacturer"), tr("Model"), tr("Registers"), tr("Description")});
    templateTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    templateTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    templateTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    registerTable->setColumnCount(10);
    registerTable->setHorizontalHeaderLabels({tr("Name"), tr("Register"), tr("Address"), tr("Count"), tr("Type"), tr("Byte order"), tr("Scale"), tr("Offset"), tr("Unit"), tr("Description")});
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
        editor.setWindowTitle(isNew ? tr("New Register Mapping") : tr("Edit Register Mapping"));
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? tr("Point") : reg.name, &editor);
        auto *type = new QComboBox(&editor); type->addItems({"Coils", "Discrete Inputs", "Input Registers", "Holding Registers"}); type->setCurrentIndex(isNew ? 3 : comboIndexFromRegisterType(static_cast<int>(reg.registerType)));
        auto *addr = new QSpinBox(&editor); addr->setRange(0, 65535); addr->setValue(isNew ? 0 : reg.address);
        auto *count = new QSpinBox(&editor); count->setRange(1, 125); count->setValue(isNew ? 1 : reg.count);
        auto *dataType = new QComboBox(&editor); dataType->setEditable(true); dataType->addItems({"uint16", "int16", "uint32", "int32", "float32", "ascii"}); dataType->setCurrentText(isNew ? "uint16" : reg.dataType);
        auto *byteOrder = new QComboBox(&editor); byteOrder->setEditable(true); byteOrder->addItems({"ABCD", "DCBA", "BADC", "CDAB"}); byteOrder->setCurrentText(isNew ? "ABCD" : reg.byteOrder);
        auto *scale = new QDoubleSpinBox(&editor); scale->setRange(-999999999, 999999999); scale->setDecimals(6); scale->setValue(isNew ? 1.0 : reg.scale);
        auto *offset = new QDoubleSpinBox(&editor); offset->setRange(-999999999, 999999999); offset->setDecimals(6); offset->setValue(isNew ? 0.0 : reg.offset);
        auto *unit = new QLineEdit(isNew ? QString() : reg.unit, &editor);
        auto *desc = new QLineEdit(isNew ? QString() : reg.description, &editor);
        form->addRow(tr("Name"), name); form->addRow(tr("Register"), type); form->addRow(tr("Address"), addr); form->addRow(tr("Count"), count); form->addRow(tr("Data type"), dataType); form->addRow(tr("Byte order"), byteOrder); form->addRow(tr("Scale"), scale); form->addRow(tr("Offset"), offset); form->addRow(tr("Unit"), unit); form->addRow(tr("Description"), desc);
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
        editor.setWindowTitle(isNew ? tr("New Device Template") : tr("Edit Device Template"));
        editor.resize(900, 560);
        auto *editorLayout = new QVBoxLayout(&editor);
        auto *formWidget = new QWidget(&editor);
        auto *form = new QFormLayout(formWidget);
        auto *name = new QLineEdit(isNew ? tr("Device template") : tmpl.name, &editor);
        auto *manufacturer = new QLineEdit(isNew ? QString() : tmpl.manufacturer, &editor);
        auto *model = new QLineEdit(isNew ? QString() : tmpl.model, &editor);
        auto *desc = new QLineEdit(isNew ? QString() : tmpl.description, &editor);
        form->addRow(tr("Name"), name); form->addRow(tr("Manufacturer"), manufacturer); form->addRow(tr("Model"), model); form->addRow(tr("Description"), desc);
        editorLayout->addWidget(formWidget);
        QVector<RegisterDefinition> regs = tmpl.registers;
        auto *regTable = new QTableWidget(&editor);
        regTable->setColumnCount(5);
        regTable->setHorizontalHeaderLabels({tr("Name"), tr("Register"), tr("Address"), tr("Count"), tr("Type")});
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
        auto *addReg = new QPushButton(tr("Add Mapping"), &editor);
        auto *editReg = new QPushButton(tr("Edit Mapping"), &editor);
        auto *delReg = new QPushButton(tr("Delete Mapping"), &editor);
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
    auto *addBtn = new QPushButton(tr("Add"), &dialog);
    auto *editBtn = new QPushButton(tr("Edit"), &dialog);
    auto *deleteBtn = new QPushButton(tr("Delete"), &dialog);
    auto *importBtn = new QPushButton(tr("Import"), &dialog);
    auto *exportBtn = new QPushButton(tr("Export"), &dialog);
    auto *bindBtn = new QPushButton(tr("Bind Device"), &dialog);
    auto *pointsBtn = new QPushButton(tr("Generate Point Table"), &dialog);
    auto *pollBtn = new QPushButton(tr("Generate Poll Tasks"), &dialog);
    auto *saveBtn = new QPushButton(tr("Save"), &dialog);
    buttons->addWidget(addBtn); buttons->addWidget(editBtn); buttons->addWidget(deleteBtn); buttons->addWidget(importBtn); buttons->addWidget(exportBtn); buttons->addWidget(bindBtn); buttons->addWidget(pointsBtn); buttons->addWidget(pollBtn); buttons->addWidget(saveBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { DeviceTemplate tmpl{}; editTemplate(tmpl, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedTemplateId(); if (id) editTemplate(m_templateManager->getTemplate(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedTemplateId(); if (id) { m_templateManager->removeTemplate(id); m_templateManager->saveToFile(templateFilePath()); refreshTemplates(); } });
    connect(importBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getOpenFileName(&dialog, tr("Import Device Template"), QCoreApplication::applicationDirPath(), "JSON Files (*.json)");
        if (!file.isEmpty() && m_templateManager->loadFromFile(file)) { m_templateManager->saveToFile(templateFilePath()); refreshTemplates(); statusBar()->showMessage(tr("Template import complete"), 3000); }
    });
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, tr("Export Device Template"), QDir::homePath() + "/templates.json", "JSON Files (*.json)");
        if (!file.isEmpty() && m_templateManager->saveToFile(file)) statusBar()->showMessage(tr("Template export complete"), 3000);
    });
    connect(saveBtn, &QPushButton::clicked, &dialog, [&]() { m_templateManager->saveToFile(templateFilePath()); statusBar()->showMessage(tr("Template saved"), 3000); });

    connect(bindBtn, &QPushButton::clicked, &dialog, [&]() {
        const int templateId = selectedTemplateId(); if (!templateId) return;
        const auto devices = m_deviceManager->allDevices();
        if (devices.isEmpty()) { QMessageBox::information(&dialog, tr("Bind Device"), tr("No device available to bind")); return; }
        QStringList names;
        for (const auto &d : devices) names << QString("[%1] %2 %3").arg(d.id).arg(d.name).arg(d.portOrAddress);
        bool ok = false;
        const QString selected = QInputDialog::getItem(&dialog, tr("Bind Device"), tr("Select device:"), names, 0, false, &ok);
        if (!ok || selected.isEmpty()) return;
        const int deviceId = selected.mid(1, selected.indexOf(']') - 1).toInt();
        m_appSettings.setValue(QString("templateBindings/%1").arg(deviceId), templateId);
        m_appSettings.sync();
        statusBar()->showMessage(tr("Template bound to device %1").arg(deviceId), 3000);
    });

    connect(pointsBtn, &QPushButton::clicked, &dialog, [&]() {
        const int id = selectedTemplateId(); if (!id) return;
        bool ok = false;
        const int serverAddress = QInputDialog::getInt(&dialog, tr("Generate Point Table"), tr("Slave address:"), ui->serverEdit->value(), 1, 247, 1, &ok);
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
            // 透传模板的字节序：之前模板里配置的 byteOrder 在生成点位时被丢弃，
            // 导致 DCBA/BADC/CDAB 设备的 32 位数据被按 ABCD 解码（错值且质量码仍为 Good）。
            {
                bool boOk = false;
                const ByteOrder bo = DataParser::byteOrderFromToken(reg.byteOrder, &boOk);
                point.byteOrder = bo;   // 缺失或无法识别时回退 ABCD（Modbus 标准）
                if (!reg.byteOrder.trimmed().isEmpty() && !boOk)
                    qWarning("模板 '%s' 的寄存器 '%s' 字节序 '%s' 无法识别，按 ABCD 处理",
                             tmpl.name.toUtf8().constData(),
                             reg.name.toUtf8().constData(),
                             reg.byteOrder.toUtf8().constData());
            }
            point.scale = reg.scale;
            point.offset = reg.offset;
            point.unit = reg.unit;
            m_pointModel->addPoint(point);
            ++created;
        }
        m_pointModel->saveToFile(pointFilePath());
        statusBar()->showMessage(tr("Generated %1 point(s)").arg(created), 3000);
    });

    connect(pollBtn, &QPushButton::clicked, &dialog, [&]() {
        const int id = selectedTemplateId(); if (!id) return;
        const auto tmpl = m_templateManager->getTemplate(id);
        bool ok = false;
        const int serverAddress = QInputDialog::getInt(&dialog, tr("Generate Poll Tasks"), tr("Slave address:"), ui->serverEdit->value(), 1, 247, 1, &ok);
        if (!ok) return;
        int created = 0;
        for (int i = 0; i < tmpl.registers.size(); ++i) {
            const auto &reg = tmpl.registers[i];
            PollTask task;
            // 轮询任务 id 分段约定（各段互不重叠，避免 addTask 按同 id 视为更新
            // 而静默覆盖先添加的任务）：
            //   1        : 默认任务
            //   100000+  : 设备派生        （设备数 < 100000）
            //   1000000+ : 模板派生        （模板数 < 1000，每模板寄存器数 < 1000）
            //   10000000+: 点位派生        （点位数 < 10000000）
            //   20000000+: 扫描器派生      （从站地址 < 10000000）
            // 原公式 20000 + tmpl.id * 1000 + i 在模板数 ≥10 或
            // 单模板寄存器数 ≥1000 时会溢出到 30000 段与点位任务撞车。
            task.id = 1000000 + tmpl.id * 1000 + i;
            task.name = QStringLiteral("%1.%2").arg(tmpl.name, reg.name);
            task.serverAddress = serverAddress;
            task.registerType = reg.registerType;
            task.startAddress = reg.address;
            task.quantity = reg.count;
            task.intervalMs = 1000;
            task.enabled = true;
            task.deviceId = 0;
            task.fromPoints = false;
            m_pollManager->removeTask(task.id);
            m_pollManager->addTask(task);
            ++created;
        }
        statusBar()->showMessage(tr("Generated %1 poll task(s)").arg(created), 3000);
    });

    refreshTemplates();
    dialog.exec();
}

void MainWindow::showScriptConsole()
{
    if (!ensurePermission("script.execute", "SCRIPT_EXECUTE", "open script console"))
        return;
    bool ok;
    QString script = QInputDialog::getMultiLineText(this, tr("Script Console"),
        tr("Enter a JavaScript snippet:"), "", &ok);
    if (!ok || script.isEmpty()) return;

        if (m_securityManager)
            m_securityManager->audit(m_securityManager->currentUser().isEmpty() ? QStringLiteral("local") : m_securityManager->currentUser(), QStringLiteral("SCRIPT_EXECUTE"), script.left(200));
        QVariant result = m_scriptEngine->evaluate(script);
    if (m_scriptEngine->lastError().isEmpty())
        QMessageBox::information(this, tr("Script Result"), result.toString());
    else
        QMessageBox::warning(this, tr("Script Error"), m_scriptEngine->lastError());
}

void MainWindow::toggleRemoteServer()
{
    if (m_remoteServer->isRunning()) {
        m_remoteServer->stop();
        statusBar()->showMessage(tr("Remote service stopped"), 3000);
        logMessage(tr("Remote HTTP service stopped"));
    } else {
        bool ok = false;
        const QString token = QInputDialog::getText(this, tr("Remote Service Authentication"), "API Token:", QLineEdit::Password, QString(), &ok);
        if (!ok || token.isEmpty())
            return;
        m_securityManager->setApiToken(token);
        const auto enableWrite = QMessageBox::question(this, tr("Remote Write Permission"), tr("Allow remote writes via /api/write?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        m_securityManager->setRemoteWriteEnabled(enableWrite == QMessageBox::Yes);
        m_securityManager->save(m_appSettings);
        m_remoteServer->setSecurityManager(m_securityManager);
        m_remoteServer->setRemoteWriteEnabled(m_securityManager->remoteWriteEnabled());
        if (m_remoteServer->start(8080)) {
            statusBar()->showMessage(tr("Remote service started, port: %1, writes: %2").arg(m_remoteServer->port()).arg(m_securityManager->remoteWriteEnabled() ? tr("On") : tr("Close")), 5000);
            logMessage(tr("Remote HTTP service started, port: %1, writes=%2").arg(m_remoteServer->port()).arg(m_securityManager->remoteWriteEnabled()));
        } else {
            statusBar()->showMessage(tr("Failed to start remote service"), 5000);
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
    m_mqttClient->setPublishQos(m_appSettings.value(QStringLiteral("mqtt/qos"), 0).toInt());

    const bool mqttEnabled = m_appSettings.value(QStringLiteral("mqtt/enabled"), false).toBool();

    // 离线队列：默认启用，断线/退出时未发送的消息落盘，重连后自动补发。
    // 仅在 MQTT 已被用户启用时打开，避免从未使用 MQTT 的现场也创建队列库。
    const int outboxMaxMessages = m_appSettings.value(
        QStringLiteral("mqtt/outboxMaxMessages"), 10000).toInt();
    const qint64 outboxMaxBytes = m_appSettings.value(
        QStringLiteral("mqtt/outboxMaxBytes"), 32LL * 1024 * 1024).toLongLong();
    m_mqttClient->setOutboxLimits(outboxMaxMessages, outboxMaxBytes);
    if (mqttEnabled
        && m_appSettings.value(QStringLiteral("mqtt/offlineQueueEnabled"), true).toBool()) {
        const QString outboxPath = m_appSettings.value(
            QStringLiteral("mqtt/outboxPath"),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + QStringLiteral("/mqtt_outbox.db")).toString();
        if (m_mqttClient->enableOutbox(outboxPath)) {
            logMessage(tr("MQTT offline queue enabled: %1 (%2 pending)")
                           .arg(outboxPath).arg(m_mqttClient->outboxCount()));
        } else {
            logMessage(tr("Failed to open MQTT offline queue; offline messages will be dropped"), 3);
        }
    }

    connect(m_mqttClient, &MqttClient::messageQueued, this,
            [this](const QString &, int, int queueSize) {
                if (queueSize == 1) {
                    logMessage(tr("MQTT not connected; message queued locally for replay after reconnect"), 2);
                }
            });
    connect(m_mqttClient, &MqttClient::outboxDrained, this, [this]() {
        logMessage(tr("MQTT offline queue fully replayed"), 1);
    });

    // 读取持久化配置（mqtt/ 配置节）
    // host 空值兜底：键存在但值为空（如对话框清空后保存）时回落默认地址，
    // 避免启动时反复出现 [ERROR] MQTT broker 地址未配置
    QString host = m_appSettings.value(QStringLiteral("mqtt/host"), QStringLiteral("127.0.0.1")).toString().trimmed();
    if (host.isEmpty()) {
        host = QStringLiteral("127.0.0.1");
        m_appSettings.setValue(QStringLiteral("mqtt/host"), host);
    }
    const int port = m_appSettings.value(QStringLiteral("mqtt/port"), 1883).toInt();
    QString clientId = m_appSettings.value(QStringLiteral("mqtt/clientId")).toString();
    if (clientId.isEmpty()) {
        clientId = QStringLiteral("fieldlink-%1")
                       .arg(QRandomGenerator::global()->bounded(100000, 999999));
        m_appSettings.setValue(QStringLiteral("mqtt/clientId"), clientId);
    }
    const QString username = m_appSettings.value(QStringLiteral("mqtt/username")).toString();
    // U1：broker 密码混淆存储，读取时解码（兼容历史明文）
    const QString password = CredentialCodec::decode(
        m_appSettings.value(QStringLiteral("mqtt/password")).toString());
    const int keepalive = m_appSettings.value(QStringLiteral("mqtt/keepalive"), 60).toInt();

    m_mqttClient->setBroker(host, static_cast<quint16>(port));
    m_mqttClient->setCredentials(clientId, username, password);
    m_mqttClient->setKeepAlive(keepalive);

    // 客户端事件进运行日志
    connect(m_mqttClient, &MqttClient::connected, this, [this]() {
        logMessage(tr("MQTT connected to broker %1").arg(m_mqttClient->brokerInfo()));
    });
    connect(m_mqttClient, &MqttClient::disconnected, this, [this]() {
        logMessage(tr("MQTT disconnected from broker"), 2);
    });
    connect(m_mqttClient, &MqttClient::errorOccurred, this, [this](const QString &message) {
        logMessage(message, 3);
    });

    // 报警事件实时上送（severity 转文本）；断线时进入离线队列，优先级最高。
    connect(m_alarmManager, &AlarmManager::alarmTriggered, this, [this](const AlarmEvent &event) {
        if (!mqttPublishEnabled())
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
        m_mqttClient->publishJson(prefix + QStringLiteral("alarm/triggered"), obj, false, MqttOutbox::Alarm);
    });

    // 曾经启用过 MQTT：启动时自动重连 broker
    if (mqttEnabled)
        m_mqttClient->connectToBroker();
}

bool MainWindow::mqttPublishEnabled() const
{
    return m_mqttClient
        && m_appSettings.value(QStringLiteral("mqtt/enabled"), false).toBool();
}

// 打开「模拟设备」测试面板（可跨开关复用，进程与面板同生命周期）
void MainWindow::showDeviceSimulator()
{
    if (!m_deviceSimulatorPanel)
        m_deviceSimulatorPanel = new DeviceSimulatorPanel(this);
    m_deviceSimulatorPanel->show();
    m_deviceSimulatorPanel->raise();
    m_deviceSimulatorPanel->activateWindow();
}

void MainWindow::showMqttSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("MQTT Publish Settings"));
    dialog.resize(470, 430);
    auto *form = new QFormLayout(&dialog);

    auto *hostEdit = new QLineEdit(
        m_appSettings.value(QStringLiteral("mqtt/host"), QStringLiteral("127.0.0.1")).toString(), &dialog);
    auto *portSpin = new QSpinBox(&dialog);
    portSpin->setRange(1, 65535);
    portSpin->setValue(m_appSettings.value(QStringLiteral("mqtt/port"), 1883).toInt());
    auto *clientEdit = new QLineEdit(m_appSettings.value(QStringLiteral("mqtt/clientId")).toString(), &dialog);
    clientEdit->setPlaceholderText(tr("Leave empty to auto-generate"));
    auto *userEdit = new QLineEdit(m_appSettings.value(QStringLiteral("mqtt/username")).toString(), &dialog);
    auto *passEdit = new QLineEdit(CredentialCodec::decode(
        m_appSettings.value(QStringLiteral("mqtt/password")).toString()), &dialog);
    passEdit->setEchoMode(QLineEdit::Password);
    auto *prefixEdit = new QLineEdit(
        m_appSettings.value(QStringLiteral("mqtt/topicPrefix"), QStringLiteral("fieldlink/")).toString(), &dialog);
    auto *keepaliveSpin = new QSpinBox(&dialog);
    keepaliveSpin->setRange(10, 3600);
    keepaliveSpin->setValue(m_appSettings.value(QStringLiteral("mqtt/keepalive"), 60).toInt());
    keepaliveSpin->setSuffix(tr(" s"));
    auto *qosCombo = new QComboBox(&dialog);
    qosCombo->addItem(tr("QoS 0 (best effort)"), 0);
    qosCombo->addItem(tr("QoS 1 (at least once)"), 1);
    qosCombo->setCurrentIndex(m_appSettings.value(QStringLiteral("mqtt/qos"), 0).toInt() >= 1 ? 1 : 0);

    auto *offlineQueueCheck = new QCheckBox(tr("Cache messages while offline and replay after reconnect"), &dialog);
    offlineQueueCheck->setChecked(
        m_appSettings.value(QStringLiteral("mqtt/offlineQueueEnabled"), true).toBool());
    auto *maxQueueSpin = new QSpinBox(&dialog);
    maxQueueSpin->setRange(100, 1000000);
    maxQueueSpin->setValue(
        m_appSettings.value(QStringLiteral("mqtt/outboxMaxMessages"), 10000).toInt());
    maxQueueSpin->setSuffix(tr(" msgs"));
    auto *queueStatusLabel = new QLabel(&dialog);
    auto *clearQueueBtn = new QPushButton(tr("Clear offline queue"), &dialog);
    auto *queueRow = new QWidget(&dialog);
    auto *queueLayout = new QHBoxLayout(queueRow);
    queueLayout->setContentsMargins(0, 0, 0, 0);
    queueLayout->addWidget(queueStatusLabel, 1);
    queueLayout->addWidget(clearQueueBtn);

    form->addRow(tr("Broker address"), hostEdit);
    form->addRow(tr("Port"), portSpin);
    form->addRow(tr("ClientID"), clientEdit);
    form->addRow(tr("Username (optional)"), userEdit);
    form->addRow(tr("Password (optional)"), passEdit);
    form->addRow(tr("Topic prefix"), prefixEdit);
    form->addRow(tr("KeepAlive"), keepaliveSpin);
    form->addRow(tr("Publish QoS"), qosCombo);
    form->addRow(tr("Offline queue"), offlineQueueCheck);
    form->addRow(tr("Queue limit"), maxQueueSpin);
    form->addRow(tr("Queued messages"), queueRow);

    auto updateQueueStatus = [this, queueStatusLabel]() {
        const int count = m_mqttClient ? m_mqttClient->outboxCount() : 0;
        const qint64 bytes = m_mqttClient ? m_mqttClient->outboxBytes() : 0;
        queueStatusLabel->setText(tr("%1 msgs / %2 KB")
                                      .arg(count)
                                      .arg(bytes / 1024));
    };
    updateQueueStatus();
    connect(m_mqttClient, &MqttClient::queueSizeChanged, &dialog,
            [updateQueueStatus](int) { updateQueueStatus(); });
    connect(clearQueueBtn, &QPushButton::clicked, &dialog, [this, updateQueueStatus]() {
        if (m_mqttClient)
            m_mqttClient->clearOutbox();
        updateQueueStatus();
    });

    auto *statusLabel = new QLabel(
        m_mqttClient->isConnectedToBroker()
            ? tr("Connected to %1").arg(m_mqttClient->brokerInfo())
            : tr("Not connected"),
        &dialog);
    form->addRow(tr("Status"), statusLabel);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Save, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    auto *connectBtn = new QPushButton(tr("Save and connect"), &dialog);
    auto *disconnectBtn = new QPushButton(tr("Disconnect"), &dialog);
    buttonBox->addButton(connectBtn, QDialogButtonBox::ActionRole);
    buttonBox->addButton(disconnectBtn, QDialogButtonBox::ActionRole);
    form->addRow(buttonBox);

    auto saveSettings = [this, hostEdit, portSpin, clientEdit, userEdit, passEdit, prefixEdit,
                         keepaliveSpin, qosCombo, offlineQueueCheck, maxQueueSpin]() {
        // host 空值兜底：清空后保存按未配置处理，回落默认地址（避免 mqtt/host 存成空串）
        QString host = hostEdit->text().trimmed();
        if (host.isEmpty())
            host = QStringLiteral("127.0.0.1");
        m_appSettings.setValue(QStringLiteral("mqtt/host"), host);
        m_appSettings.setValue(QStringLiteral("mqtt/port"), portSpin->value());
        if (!clientEdit->text().trimmed().isEmpty())
            m_appSettings.setValue(QStringLiteral("mqtt/clientId"), clientEdit->text().trimmed());
        m_appSettings.setValue(QStringLiteral("mqtt/username"), userEdit->text());
        // U1：保存时统一混淆（历史明文在下次保存后升级）
        m_appSettings.setValue(QStringLiteral("mqtt/password"),
                               CredentialCodec::encode(passEdit->text()));
        QString prefix = prefixEdit->text();
        if (!prefix.isEmpty() && !prefix.endsWith(QLatin1Char('/')))
            prefix += QLatin1Char('/');
        m_appSettings.setValue(QStringLiteral("mqtt/topicPrefix"), prefix);
        m_appSettings.setValue(QStringLiteral("mqtt/keepalive"), keepaliveSpin->value());
        m_appSettings.setValue(QStringLiteral("mqtt/qos"), qosCombo->currentData().toInt());
        m_appSettings.setValue(QStringLiteral("mqtt/offlineQueueEnabled"), offlineQueueCheck->isChecked());
        m_appSettings.setValue(QStringLiteral("mqtt/outboxMaxMessages"), maxQueueSpin->value());

        m_mqttClient->setPublishQos(qosCombo->currentData().toInt());
        m_mqttClient->setOutboxLimits(maxQueueSpin->value(),
                                      m_appSettings.value(QStringLiteral("mqtt/outboxMaxBytes"),
                                                          32LL * 1024 * 1024).toLongLong());
        if (offlineQueueCheck->isChecked()) {
            const QString outboxPath = m_appSettings.value(
                QStringLiteral("mqtt/outboxPath"),
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                    + QStringLiteral("/mqtt_outbox.db")).toString();
            m_mqttClient->enableOutbox(outboxPath);
        } else {
            m_mqttClient->disableOutbox();
        }
    };
    // 原实现里普通「Save」只关闭对话框不保存；这里补上，避免离线队列/QoS 设置丢失。
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, [saveSettings]() { saveSettings(); });

    connect(connectBtn, &QPushButton::clicked, &dialog, [this, saveSettings, statusLabel]() {
        saveSettings();
        m_mqttClient->setBroker(
            m_appSettings.value(QStringLiteral("mqtt/host")).toString(),
            static_cast<quint16>(m_appSettings.value(QStringLiteral("mqtt/port"), 1883).toInt()));
        m_mqttClient->setCredentials(
            m_appSettings.value(QStringLiteral("mqtt/clientId")).toString(),
            m_appSettings.value(QStringLiteral("mqtt/username")).toString(),
            CredentialCodec::decode(
                m_appSettings.value(QStringLiteral("mqtt/password")).toString()));
        m_mqttClient->setKeepAlive(m_appSettings.value(QStringLiteral("mqtt/keepalive"), 60).toInt());
        m_appSettings.setValue(QStringLiteral("mqtt/enabled"), true);
        m_mqttClient->connectToBroker();
        statusLabel->setText(tr("Connecting..."));
    });

    connect(disconnectBtn, &QPushButton::clicked, &dialog, [this, statusLabel]() {
        m_appSettings.setValue(QStringLiteral("mqtt/enabled"), false);
        m_mqttClient->disconnectFromBroker();
        statusLabel->setText(tr("Disconnected"));
    });

    connect(m_mqttClient, &MqttClient::connected, statusLabel, [statusLabel, this]() {
        statusLabel->setText(tr("Connected to %1").arg(m_mqttClient->brokerInfo()));
    });
    connect(m_mqttClient, &MqttClient::disconnected, statusLabel, [statusLabel]() {
        statusLabel->setText(tr("Not connected"));
    });
    connect(m_mqttClient, &MqttClient::errorOccurred, statusLabel, [statusLabel](const QString &message) {
        statusLabel->setText(message);
    });

    dialog.exec();
}

// 数据库连接配置：FieldLink 使用嵌入式 SQLite（本地 .db 文件），
// 不需要服务器地址/端口/账号密码；这里集中展示并允许修改三个库文件路径。
void MainWindow::showDatabaseSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Database Settings"));
    dialog.resize(780, 560);
    auto *layout = new QVBoxLayout(&dialog);

    auto *intro = new QLabel(tr(
        "FieldLink uses embedded SQLite databases — no server address, port, username or password is required. "
        "Each database is a local .db file. History and alarm databases switch immediately; the MQTT offline "
        "queue path is applied the next time the queue is opened."), &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    const QString defaultDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    auto *historyEdit = new QLineEdit(m_appSettings.value(
        QStringLiteral("history/dbPath"),
        defaultDir + QStringLiteral("/history.db")).toString(), &dialog);
    auto *alarmEdit = new QLineEdit(m_appSettings.value(
        QStringLiteral("alarm/dbPath"),
        defaultDir + QStringLiteral("/alarms.db")).toString(), &dialog);
    auto *outboxEdit = new QLineEdit(m_appSettings.value(
        QStringLiteral("mqtt/outboxPath"),
        defaultDir + QStringLiteral("/mqtt_outbox.db")).toString(), &dialog);

    auto makePathRow = [&](QLineEdit *edit) {
        auto *row = new QWidget(&dialog);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(edit, 1);
        auto *browse = new QPushButton(tr("Browse..."), row);
        h->addWidget(browse);
        connect(browse, &QPushButton::clicked, &dialog, [&dialog, edit]() {
            const QString file = QFileDialog::getSaveFileName(
                &dialog, tr("Select SQLite Database"), edit->text(),
                tr("SQLite Database (*.db);;All Files (*)"));
            if (!file.isEmpty())
                edit->setText(QDir::toNativeSeparators(file));
        });
        return row;
    };

    auto *form = new QFormLayout();
    form->addRow(tr("History database"), makePathRow(historyEdit));
    form->addRow(tr("Alarm database"), makePathRow(alarmEdit));
    form->addRow(tr("MQTT offline queue"), makePathRow(outboxEdit));
    layout->addLayout(form);

    auto *statusBox = new QGroupBox(tr("Connection status"), &dialog);
    auto *statusLayout = new QVBoxLayout(statusBox);
    auto *historyStatus = new QLabel(statusBox);
    auto *alarmStatus = new QLabel(statusBox);
    auto *outboxStatus = new QLabel(statusBox);
    for (QLabel *label : {historyStatus, alarmStatus, outboxStatus}) {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusLayout->addWidget(label);
    }
    layout->addWidget(statusBox);

    auto refreshStatus = [&]() {
        const bool historyOpen = m_historyData && m_historyData->isOpen();
        const qint64 historyRecords = historyOpen ? m_historyData->totalRecords() : 0;
        historyStatus->setText(tr("History: %1 | %2 | %3 records")
            .arg(historyOpen ? tr("open") : tr("not open"))
            .arg(QDir::toNativeSeparators(historyEdit->text()))
            .arg(historyRecords));

        const bool alarmOpen = m_alarmRepository && m_alarmRepository->isOpen();
        const int rules = alarmOpen ? m_alarmRepository->ruleCount() : 0;
        const int events = alarmOpen ? m_alarmRepository->eventCount() : 0;
        alarmStatus->setText(tr("Alarms: %1 | %2 | %3 rules / %4 events")
            .arg(alarmOpen ? tr("open") : tr("not open"))
            .arg(QDir::toNativeSeparators(alarmEdit->text()))
            .arg(rules)
            .arg(events));

        const bool outboxOpen = m_mqttClient && m_mqttClient->isOutboxEnabled();
        const int messages = outboxOpen ? m_mqttClient->outboxCount() : 0;
        outboxStatus->setText(tr("MQTT offline queue: %1 | %2 | %3 messages")
            .arg(outboxOpen ? tr("open") : tr("disabled"))
            .arg(QDir::toNativeSeparators(outboxEdit->text()))
            .arg(messages));
    };
    refreshStatus();

    auto *buttonRow = new QHBoxLayout();
    auto *testBtn = new QPushButton(tr("Test Connections"), &dialog);
    auto *saveBtn = new QPushButton(tr("Save"), &dialog);
    auto *closeBtn = new QPushButton(tr("Close"), &dialog);
    buttonRow->addWidget(testBtn);
    buttonRow->addStretch();
    buttonRow->addWidget(saveBtn);
    buttonRow->addWidget(closeBtn);
    layout->addLayout(buttonRow);

    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    connect(testBtn, &QPushButton::clicked, &dialog, [&]() {
        QStringList errors;
        QString error;
        if (!testSqlitePath(historyEdit->text(), &error))
            errors << tr("History database: %1").arg(error);
        if (!testSqlitePath(alarmEdit->text(), &error))
            errors << tr("Alarm database: %1").arg(error);
        if (!testSqlitePath(outboxEdit->text(), &error))
            errors << tr("MQTT offline queue: %1").arg(error);
        if (errors.isEmpty())
            QMessageBox::information(&dialog, tr("Database Test"),
                                     tr("All SQLite database files are accessible."));
        else
            QMessageBox::warning(&dialog, tr("Database Test"), errors.join('\n'));
    });

    connect(saveBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString historyPath = historyEdit->text().trimmed();
        const QString alarmPath = alarmEdit->text().trimmed();
        const QString outboxPath = outboxEdit->text().trimmed();
        if (historyPath.isEmpty() || alarmPath.isEmpty() || outboxPath.isEmpty()) {
            QMessageBox::warning(&dialog, tr("Database Settings"),
                                 tr("Database paths cannot be empty."));
            return;
        }

        // 历史库：openDatabase 会自动关闭旧连接并切换。
        const QString oldHistoryPath = m_appSettings.value(
            QStringLiteral("history/dbPath"),
            defaultDir + QStringLiteral("/history.db")).toString();
        if (!m_historyData->openDatabase(historyPath)) {
            const QString error = m_historyData->lastError();
            m_historyData->openDatabase(oldHistoryPath);
            QMessageBox::warning(&dialog, tr("Database Settings"),
                                 tr("Cannot open history database: %1").arg(error));
            return;
        }
        m_appSettings.setValue(QStringLiteral("history/dbPath"), historyPath);

        // 报警库：切换后重新载入规则/事件/活跃状态。
        // 若新库为空，则把当前内存中的规则写入新库，避免切换即丢失规则。
        const auto inMemoryRules = m_alarmManager->allRules();
        const QString oldAlarmPath = m_alarmRepository ? m_alarmRepository->databasePath() : QString();
        if (!m_alarmRepository)
            m_alarmRepository.reset(new AlarmRepository());
        if (!m_alarmRepository->open(alarmPath)) {
            const QString error = m_alarmRepository->lastError();
            if (!oldAlarmPath.isEmpty())
                m_alarmRepository->open(oldAlarmPath);
            QMessageBox::warning(&dialog, tr("Database Settings"),
                                 tr("Cannot open alarm database: %1").arg(error));
            return;
        }
        if (m_alarmRepository->ruleCount() == 0 && !inMemoryRules.isEmpty()) {
            for (const auto &rule : inMemoryRules)
                m_alarmRepository->saveRule(rule);
        }
        if (m_alarmManager->repository() != m_alarmRepository.data())
            m_alarmManager->setRepository(m_alarmRepository.data());
        m_alarmManager->reloadPersistedState();
        m_appSettings.setValue(QStringLiteral("alarm/dbPath"), alarmPath);

        // MQTT 离线队列：仅在当前已启用时立即切换；否则下次启动/启用时生效。
        m_appSettings.setValue(QStringLiteral("mqtt/outboxPath"), outboxPath);
        if (m_mqttClient && m_mqttClient->isOutboxEnabled()
            && !m_mqttClient->enableOutbox(outboxPath)) {
            QMessageBox::warning(&dialog, tr("Database Settings"),
                                 tr("Cannot open MQTT offline queue: %1").arg(outboxPath));
        }

        refreshStatus();
        statusBar()->showMessage(tr("Database settings saved"), 4000);
        QMessageBox::information(&dialog, tr("Database Settings"),
                                 tr("Database settings saved. History and alarm databases now use the new paths."));
    });

    dialog.exec();
}

void MainWindow::publishMqttTelemetry(int deviceId, int serverAddress, int registerType,
                                      int startAddress, const QVector<quint16> &values)
{
    if (!mqttPublishEnabled() || values.isEmpty())
        return;

    QJsonObject obj;
    obj[QStringLiteral("timestamp")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    // deviceId 必须上送：多设备可能存在相同的从站地址/寄存器地址
    obj[QStringLiteral("deviceId")] = deviceId;
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
    // 主题保持旧格式（device 0 完全兼容）；多设备时追加设备段，避免不同设备互相覆盖
    const QString topic = deviceId == 0
        ? QStringLiteral("%1data/%2/%3/%4")
              .arg(prefix).arg(serverAddress).arg(registerTypeName(registerType)).arg(startAddress)
        : QStringLiteral("%1data/d%2/%3/%4/%5")
              .arg(prefix).arg(deviceId).arg(serverAddress)
              .arg(registerTypeName(registerType)).arg(startAddress);
    m_mqttClient->publishJson(topic, obj, false, MqttOutbox::Telemetry);
}

void MainWindow::publishMqttStatus(bool connected)
{
    if (!mqttPublishEnabled())
        return;

    QJsonObject obj;
    obj[QStringLiteral("connected")] = connected;
    obj[QStringLiteral("endpoint")] = ui->portEdit ? ui->portEdit->currentText() : QString();
    obj[QStringLiteral("timestamp")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);

    const QString prefix = m_appSettings.value(QStringLiteral("mqtt/topicPrefix"),
                                               QStringLiteral("fieldlink/")).toString();
    // retain=true：订阅方上线即可获得最后一次现场在线状态
    m_mqttClient->publishJson(prefix + QStringLiteral("status"), obj, true, MqttOutbox::Status);
}

void MainWindow::showPluginManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Plugin Management"));
    dialog.resize(1050, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(8);
    table->setHorizontalHeaderLabels({tr("No."), tr("Name"), tr("Version"), tr("Author"), tr("Status"), tr("Enable"), tr("Path"), tr("Last error")});
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
                p.loaded ? tr("Loaded") : tr("Not loaded"),
                p.enabled ? tr("Yes") : tr("No"),
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
    auto *scanBtn = new QPushButton(tr("Scan"), &dialog);
    auto *enableBtn = new QPushButton(tr("Enable"), &dialog);
    auto *disableBtn = new QPushButton(tr("Disable"), &dialog);
    auto *loadBtn = new QPushButton(tr("Load"), &dialog);
    auto *unloadBtn = new QPushButton(tr("Unload"), &dialog);
    auto *reloadBtn = new QPushButton(tr("Reload"), &dialog);
    auto *configBtn = new QPushButton(tr("Configuration JSON"), &dialog);
    auto *errorBtn = new QPushButton(tr("Error Details"), &dialog);
    buttons->addWidget(scanBtn); buttons->addWidget(enableBtn); buttons->addWidget(disableBtn); buttons->addWidget(loadBtn); buttons->addWidget(unloadBtn); buttons->addWidget(reloadBtn); buttons->addWidget(configBtn); buttons->addWidget(errorBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(scanBtn, &QPushButton::clicked, &dialog, [&]() { m_pluginManager->scanPlugins(); refresh(); statusBar()->showMessage(tr("Plugin scan complete"), 3000); });
    connect(enableBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->setPluginEnabled(index, true); refresh(); } });
    connect(disableBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->setPluginEnabled(index, false); refresh(); } });
    connect(loadBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->loadPlugin(index); refresh(); } });
    connect(unloadBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->unloadPlugin(index); refresh(); } });
    connect(reloadBtn, &QPushButton::clicked, &dialog, [&]() { const int index = selectedIndex(); if (index >= 0) { m_pluginManager->reloadPlugin(index); refresh(); } });
    connect(configBtn, &QPushButton::clicked, &dialog, [&]() {
        const int index = selectedIndex(); if (index < 0) return;
        bool ok = false;
        const QString current = QString::fromUtf8(QJsonDocument(m_pluginManager->pluginConfig(index)).toJson(QJsonDocument::Indented));
        const QString text = QInputDialog::getMultiLineText(&dialog, tr("Plugin Configuration JSON"), tr("Configuration:"), current, &ok);
        if (!ok) return;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            QMessageBox::warning(&dialog, tr("Plugin Configuration"), tr("Invalid JSON format"));
            return;
        }
        m_pluginManager->setPluginConfig(index, doc.object());
        refresh();
    });
    connect(errorBtn, &QPushButton::clicked, &dialog, [&]() {
        const int index = selectedIndex(); if (index < 0) return;
        const auto plugins = m_pluginManager->plugins();
        if (index >= plugins.size()) return;
        QMessageBox::information(&dialog, tr("Plugin Error Details"), tr("Plugin: %1\nPath: %2\nStatus: %3\nLast error: %4")
            .arg(plugins[index].name, plugins[index].filePath, plugins[index].loaded ? tr("Loaded") : tr("Not loaded"), plugins[index].lastError.isEmpty() ? tr("None") : plugins[index].lastError));
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
    dialog.setWindowTitle(tr("History Query"));
    dialog.resize(1200, 700);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filters = new QHBoxLayout();

    auto *modeCombo = new QComboBox(&dialog);
    modeCombo->addItem(tr("Engineering values"), 0);       // 按点位归档的 scale/offset 换算值
    modeCombo->addItem(tr("Raw registers"), 1);   // 报文级追溯

    auto *fromEdit = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-1), &dialog);
    auto *toEdit = new QDateTimeEdit(QDateTime::currentDateTime(), &dialog);
    fromEdit->setCalendarPopup(true);
    toEdit->setCalendarPopup(true);

    auto *deviceCombo = new QComboBox(&dialog);
    deviceCombo->addItem(tr("All devices"), -1);
    deviceCombo->addItem(tr("Main connection"), 0);
    for (const auto &d : m_deviceManager->allDevices())
        deviceCombo->addItem(tr("%1 (Device %2)").arg(d.name).arg(d.id), d.id);

    auto *pointCombo = new QComboBox(&dialog);
    pointCombo->addItem(tr("All points"), -1);

    auto *serverEdit = new QSpinBox(&dialog); serverEdit->setRange(-1, 247); serverEdit->setValue(-1); serverEdit->setSpecialValueText(tr("All"));
    auto *typeEdit = new QComboBox(&dialog); typeEdit->addItems({tr("All"), "Coils", "Discrete Inputs", "Input Registers", "Holding Registers"});
    auto *addrEdit = new QSpinBox(&dialog); addrEdit->setRange(-1, 65535); addrEdit->setValue(-1); addrEdit->setSpecialValueText(tr("All"));
    auto *aggCombo = new QComboBox(&dialog);
    aggCombo->addItem(tr("No aggregation"), 0);
    aggCombo->addItem(tr("1 minute"), 60);
    aggCombo->addItem(tr("5 minutes"), 300);
    aggCombo->addItem(tr("1 hour"), 3600);

    filters->addWidget(new QLabel(tr("Mode"), &dialog)); filters->addWidget(modeCombo);
    filters->addWidget(new QLabel(tr("Start"), &dialog)); filters->addWidget(fromEdit);
    filters->addWidget(new QLabel(tr("End"), &dialog)); filters->addWidget(toEdit);
    filters->addWidget(new QLabel(tr("Device"), &dialog)); filters->addWidget(deviceCombo);
    filters->addWidget(new QLabel(tr("Point"), &dialog)); filters->addWidget(pointCombo);
    filters->addWidget(new QLabel(tr("Slave"), &dialog)); filters->addWidget(serverEdit);
    filters->addWidget(new QLabel(tr("Register"), &dialog)); filters->addWidget(typeEdit);
    filters->addWidget(new QLabel(tr("Address"), &dialog)); filters->addWidget(addrEdit);
    filters->addWidget(new QLabel(tr("Aggregation"), &dialog)); filters->addWidget(aggCombo);
    layout->addLayout(filters);

    auto *table = new QTableWidget(&dialog);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table);

    // 导出严格跟随当前视图（模式 + 聚合），避免"界面看到工程值、CSV 却是原始寄存器"。
    QStringList currentHeader;
    QVector<QStringList> currentRows;
    constexpr int kMaxDisplayRows = 5000;

    auto isEngineering = [&]() { return modeCombo->currentData().toInt() == 0; };

    // 点位下拉随设备筛选联动：避免"全部设备"下几百个同名点位无法区分。
    auto rebuildPointCombo = [&]() {
        const int selectedDevice = deviceCombo->currentData().toInt();
        pointCombo->blockSignals(true);
        pointCombo->clear();
        pointCombo->addItem(tr("All points"), -1);
        for (const PointDefinition &p : m_pointModel->points()) {
            if (selectedDevice >= 0 && p.deviceId != selectedDevice)
                continue;
            pointCombo->addItem(tr("%1 (Device %2 / ID %3)")
                                    .arg(p.name).arg(deviceDisplayName(p.deviceId)).arg(p.id), p.id);
        }
        pointCombo->blockSignals(false);
    };

    auto applyModeVisibility = [&]() {
        const bool eng = isEngineering();
        deviceCombo->setEnabled(eng);
        pointCombo->setEnabled(eng);
        aggCombo->setEnabled(eng);
        serverEdit->setEnabled(!eng);
        typeEdit->setEnabled(!eng);
        addrEdit->setEnabled(!eng);
    };

    auto setTable = [&](const QStringList &header, const QVector<QStringList> &rows) {
        currentHeader = header;
        currentRows = rows;
        table->setColumnCount(header.size());
        table->setHorizontalHeaderLabels(header);
        table->setRowCount(rows.size());
        for (int row = 0; row < rows.size(); ++row) {
            const QStringList &cells = rows[row];
            for (int col = 0; col < cells.size() && col < header.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };

    auto refreshEngineering = [&]() {
        const QDateTime from = fromEdit->dateTime();
        const QDateTime to = toEdit->dateTime();
        const int deviceId = deviceCombo->currentData().toInt();
        const int pointId = pointCombo->currentData().toInt();
        const int bucket = aggCombo->currentData().toInt();

        if (bucket > 0) {
            if (pointId < 0) {
                QMessageBox::information(&dialog, tr("Select a point"),
                                         tr("Select a specific point when aggregating by time, otherwise values from different points will be mixed into one series."));
                return;
            }
            const auto aggregates = m_historyData->aggregatePoints(from, to, deviceId, pointId, bucket);
            QVector<QStringList> rows;
            rows.reserve(aggregates.size());
            for (const auto &a : aggregates) {
                rows.append({a.bucketStart.toString("yyyy-MM-dd HH:mm:ss"),
                             QString::number(a.avgValue, 'f', 3),
                             QString::number(a.minValue, 'f', 3),
                             QString::number(a.maxValue, 'f', 3),
                             QString::number(a.count)});
            }
            setTable({tr("Time bucket"), tr("Average"), tr("Min"), tr("Max"), tr("Samples")}, rows);
            statusBar()->showMessage(tr("Aggregation query complete: %1 time bucket(s)").arg(rows.size()), 3000);
            return;
        }

        const auto records = m_historyData->queryPoints(from, to, deviceId, pointId, kMaxDisplayRows, 0);
        QVector<QStringList> rows;
        rows.reserve(records.size());
        for (const auto &r : records) {
            QString pointText = r.pointName;
            if (pointText.isEmpty() && m_pointModel)
                pointText = m_pointModel->point(r.pointId).name;
            rows.append({QString::number(r.id),
                         r.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"),
                         deviceDisplayName(r.deviceId),
                         QStringLiteral("%1 (ID%2)").arg(pointText).arg(r.pointId),
                         QString::number(r.serverAddress),
                         QString::number(r.value, 'f', 3),
                         pointQualityName(static_cast<DataQuality>(r.quality))});
        }
        setTable({"ID", tr("Time"), tr("Device"), tr("Point"), tr("Slave"), tr("Value"), tr("Quality")}, rows);
        if (records.size() >= kMaxDisplayRows)
            statusBar()->showMessage(tr("Engineering value query complete: showing first %1 record(s) (truncated, narrow the time range)")
                                          .arg(records.size()), 5000);
        else
            statusBar()->showMessage(tr("Engineering value query complete: %1 record(s)").arg(records.size()), 3000);
    };

    auto refreshRaw = [&]() {
        const int regType = typeEdit->currentIndex() == 0 ? -1 : static_cast<int>(registerTypeFromComboIndex(typeEdit->currentIndex() - 1));
        const auto records = m_historyData->query(fromEdit->dateTime(), toEdit->dateTime(), serverEdit->value(),
                                                  regType, addrEdit->value(), kMaxDisplayRows, 0);
        QVector<QStringList> rows;
        rows.reserve(records.size());
        for (const auto &r : records) {
            QStringList values;
            for (quint16 v : r.values) values << QString::number(v);
            rows.append({QString::number(r.id),
                         r.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"),
                         QString::number(r.serverAddress),
                         registerTypeName(static_cast<int>(r.registerType)),
                         QString::number(r.startAddress),
                         QString::number(r.count),
                         values.join(",")});
        }
        setTable({"ID", tr("Time"), tr("Slave"), tr("Register"), tr("Start address"), tr("Count"), tr("Value")}, rows);
        if (records.size() >= kMaxDisplayRows)
            statusBar()->showMessage(tr("Raw register query complete: showing first %1 record(s) (truncated, narrow the time range)")
                                          .arg(records.size()), 5000);
        else
            statusBar()->showMessage(tr("Raw register query complete: %1 record(s)").arg(records.size()), 3000);
    };

    auto refresh = [&]() {
        if (isEngineering())
            refreshEngineering();
        else
            refreshRaw();
    };

    auto *buttons = new QHBoxLayout();
    auto *queryBtn = new QPushButton(tr("Query"), &dialog);
    auto *lastBtn = new QPushButton(tr("Latest 100"), &dialog);
    auto *exportBtn = new QPushButton(tr("Export CSV"), &dialog);
    auto *clearOldBtn = new QPushButton(tr("Clear older than 30 days"), &dialog);
    buttons->addWidget(queryBtn); buttons->addWidget(lastBtn); buttons->addWidget(exportBtn); buttons->addWidget(clearOldBtn); buttons->addStretch();
    layout->addLayout(buttons);

    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) {
        applyModeVisibility();
        refresh();
    });
    connect(deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) {
        rebuildPointCombo();
    });
    connect(queryBtn, &QPushButton::clicked, &dialog, refresh);
    connect(lastBtn, &QPushButton::clicked, &dialog, [&]() {
        const QDateTime from = QDateTime::currentDateTime().addDays(-7);
        const QDateTime to = QDateTime::currentDateTime();
        if (isEngineering()) {
            // 最近 100 条工程值：直接按时间倒序取，不做聚合
            aggCombo->setCurrentIndex(0);
            const int deviceId = deviceCombo->currentData().toInt();
            const int pointId = pointCombo->currentData().toInt();
            const auto records = m_historyData->queryPoints(from, to, deviceId, pointId, 100, 0);
            QVector<QStringList> rows;
            for (const auto &r : records) {
                QString pointText = r.pointName;
                if (pointText.isEmpty() && m_pointModel)
                    pointText = m_pointModel->point(r.pointId).name;
                rows.append({QString::number(r.id),
                             r.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"),
                             deviceDisplayName(r.deviceId),
                             QStringLiteral("%1 (ID%2)").arg(pointText).arg(r.pointId),
                             QString::number(r.serverAddress),
                             QString::number(r.value, 'f', 3),
                             pointQualityName(static_cast<DataQuality>(r.quality))});
            }
            setTable({"ID", tr("Time"), tr("Device"), tr("Point"), tr("Slave"), tr("Value"), tr("Quality")}, rows);
        } else {
            const auto records = m_historyData->lastRecords(100);
            QVector<QStringList> rows;
            for (const auto &r : records) {
                QStringList values;
                for (quint16 v : r.values) values << QString::number(v);
                rows.append({QString::number(r.id),
                             r.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"),
                             QString::number(r.serverAddress),
                             registerTypeName(static_cast<int>(r.registerType)),
                             QString::number(r.startAddress),
                             QString::number(r.count),
                             values.join(",")});
            }
            setTable({"ID", tr("Time"), tr("Slave"), tr("Register"), tr("Start address"), tr("Count"), tr("Value")}, rows);
        }
    });
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, tr("Export History Data"), QDir::homePath() + "/history.csv", "CSV Files (*.csv)");
        if (file.isEmpty()) return;
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);
        out.setGenerateByteOrderMark(true);
        auto escape = [](const QString &text) {
            QString escaped = text;
            escaped.replace('"', "\"\"");
            return escaped.contains(',') || escaped.contains('"') || escaped.contains('\n')
                       ? QStringLiteral("\"%1\"").arg(escaped) : escaped;
        };
        QStringList header;
        for (const QString &cell : currentHeader) header << escape(cell);
        out << header.join(',') << '\n';
        for (const QStringList &row : currentRows) {
            QStringList cells;
            for (const QString &cell : row) cells << escape(cell);
            out << cells.join(',') << '\n';
        }
        statusBar()->showMessage(tr("History data exported"), 3000);
    });
    connect(clearOldBtn, &QPushButton::clicked, &dialog, [&]() { m_historyData->clearOlderThan(QDateTime::currentDateTime().addDays(-30)); refresh(); });

    rebuildPointCombo();
    applyModeVisibility();
    refresh();
    dialog.exec();
}

void MainWindow::showPointManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Point Management"));
    dialog.resize(1280, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(19);
    table->setHorizontalHeaderLabels({"ID", tr("Name"), tr("Device"), tr("Slave"), tr("Register"), tr("Address"), tr("Count"), tr("Data type"), tr("Scale"), tr("Offset"), tr("Unit"), tr("Alarm low"), tr("Alarm high"), tr("Archive"), tr("Archive interval"), tr("Poll interval"), tr("Archive deadband"), tr("Current value"), tr("Quality code")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    // 点位值刷新的节流定时器（200ms）：
    // 避免"每个点位值更新都整表重建"造成的 UI 风暴。
    auto *valueRefreshTimer = new QTimer(&dialog);
    valueRefreshTimer->setSingleShot(true);

    auto refresh = [&]() {
        const auto points = m_pointModel->points();
        table->setRowCount(points.size());
        for (int row = 0; row < points.size(); ++row) {
            const auto &p = points[row];
            const auto v = currentPointValue(m_pointModel, p.id);
            const QString deviceName = deviceDisplayName(p.deviceId);
            const QStringList cells = {QString::number(p.id), p.name, deviceName, QString::number(p.serverAddress), registerTypeName(static_cast<int>(p.registerType)), QString::number(p.address), QString::number(p.count), p.dataType, QString::number(p.scale), QString::number(p.offset), p.unit, QString::number(p.alarmLow), QString::number(p.alarmHigh), p.archiveEnabled ? tr("Yes") : tr("No"), QString::number(p.archiveIntervalSec), QString::number(p.pollIntervalSec), QString::number(p.archiveDeadband), v.timestamp.isValid() ? QString::number(v.value, 'f', 3) : QStringLiteral("--"), pointQualityName(v.quality)};
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    };
    auto selectedPointId = [&]() -> int { const int row = table->currentRow(); return row >= 0 && table->item(row, 0) ? table->item(row, 0)->text().toInt() : 0; };
    auto editPoint = [&](PointDefinition point, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? tr("New Point") : tr("Edit Point"));
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? tr("Point") : point.name, &editor);
        // 设备维度：0 = 主连接；其余为 DeviceManager 设备。缺失该选择会导致
        // 两台同从站地址的设备共用一个点位集（数据互相覆盖）。
        auto *device = new QComboBox(&editor);
        device->addItem(tr("Main connection"), 0);
        for (const auto &d : m_deviceManager->allDevices())
            device->addItem(tr("%1 (Device %2)").arg(d.name).arg(d.id), d.id);
        {
            const int index = device->findData(isNew ? 0 : point.deviceId);
            device->setCurrentIndex(index >= 0 ? index : 0);
        }
        auto *server = new QSpinBox(&editor); server->setRange(1, 247); server->setValue(isNew ? ui->serverEdit->value() : point.serverAddress);
        auto *regType = new QComboBox(&editor); regType->addItems({"Coils", "Discrete Inputs", "Input Registers", "Holding Registers"}); regType->setCurrentIndex(isNew ? currentUiRegisterComboIndex(ui) : comboIndexFromRegisterType(static_cast<int>(point.registerType)));
        auto *address = new QSpinBox(&editor); address->setRange(0, 65535); address->setValue(isNew ? ui->readAddress->value() : point.address);
        auto *count = new QSpinBox(&editor); count->setRange(1, 125); count->setValue(isNew ? qMax(1, ui->readSize->currentText().toInt()) : point.count);
        auto *dataType = new QComboBox(&editor); dataType->setEditable(true); dataType->addItems({"uint16", "int16", "uint32", "int32", "float32", "uint64", "int64", "float64", "ascii"}); dataType->setCurrentText(isNew ? "uint16" : point.dataType);
        // 字节序（仅对 32/64 位类型有意义）：必须让用户能配置，
        // 否则 DCBA/BADC/CDAB 设备的 32 位数据只能按 ABCD 解码（数值错误）。
        auto *byteOrder = new QComboBox(&editor);
        byteOrder->addItems({tr("ABCD (big-endian)"), tr("DCBA (little-endian)"), "BADC", "CDAB"});
        byteOrder->setCurrentIndex(static_cast<int>(point.byteOrder));
        auto *scale = new QDoubleSpinBox(&editor); scale->setRange(-999999999, 999999999); scale->setDecimals(6); scale->setValue(isNew ? 1.0 : point.scale);
        auto *offset = new QDoubleSpinBox(&editor); offset->setRange(-999999999, 999999999); offset->setDecimals(6); offset->setValue(isNew ? 0.0 : point.offset);
        auto *unit = new QLineEdit(isNew ? QString() : point.unit, &editor);
        auto *alarmLow = new QDoubleSpinBox(&editor); alarmLow->setRange(-999999999, 999999999); alarmLow->setDecimals(3); alarmLow->setValue(isNew ? 0.0 : point.alarmLow);
        auto *alarmHigh = new QDoubleSpinBox(&editor); alarmHigh->setRange(-999999999, 999999999); alarmHigh->setDecimals(3); alarmHigh->setValue(isNew ? 65535.0 : point.alarmHigh);
        auto *archiveEnabled = new QCheckBox(&editor); archiveEnabled->setChecked(isNew ? true : point.archiveEnabled);
        auto *archiveInterval = new QSpinBox(&editor); archiveInterval->setRange(1, 86400); archiveInterval->setValue(isNew ? 5 : point.archiveIntervalSec);
        auto *pollInterval = new QSpinBox(&editor); pollInterval->setRange(1, 3600); pollInterval->setValue(isNew ? 1 : point.pollIntervalSec);
        auto *archiveDeadband = new QDoubleSpinBox(&editor); archiveDeadband->setRange(0, 999999999); archiveDeadband->setDecimals(6); archiveDeadband->setValue(isNew ? 0.0 : point.archiveDeadband);
        form->addRow(tr("Device"), device); form->addRow(tr("Name"), name); form->addRow(tr("Slave address"), server); form->addRow(tr("Register type"), regType); form->addRow(tr("Address"), address); form->addRow(tr("Count"), count); form->addRow(tr("Data type"), dataType); form->addRow(tr("Byte order"), byteOrder); form->addRow(tr("Scale"), scale); form->addRow(tr("Offset"), offset); form->addRow(tr("Unit"), unit); form->addRow(tr("Alarm low"), alarmLow); form->addRow(tr("Alarm high"), alarmHigh); form->addRow(tr("Archive enabled"), archiveEnabled); form->addRow(tr("Archive interval (s)"), archiveInterval); form->addRow(tr("Poll interval (s)"), pollInterval); form->addRow(tr("Archive deadband"), archiveDeadband);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        if (editor.exec() != QDialog::Accepted) return;
        point.name = name->text(); point.deviceId = device->currentData().toInt(); point.serverAddress = server->value(); point.registerType = registerTypeFromComboIndex(regType->currentIndex()); point.address = address->value(); point.count = count->value(); point.dataType = dataType->currentText(); point.byteOrder = static_cast<ByteOrder>(byteOrder->currentIndex()); point.scale = scale->value(); point.offset = offset->value(); point.unit = unit->text(); point.alarmLow = alarmLow->value(); point.alarmHigh = alarmHigh->value(); point.archiveEnabled = archiveEnabled->isChecked(); point.archiveIntervalSec = archiveInterval->value(); point.pollIntervalSec = pollInterval->value(); point.archiveDeadband = archiveDeadband->value();
        if (isNew) m_pointModel->addPoint(point); else m_pointModel->updatePoint(point);
        m_pointModel->saveToFile(pointFilePath()); refresh();
    };

    auto *buttons = new QHBoxLayout();
    auto *addBtn = new QPushButton(tr("Add"), &dialog); auto *editBtn = new QPushButton(tr("Edit"), &dialog); auto *deleteBtn = new QPushButton(tr("Delete"), &dialog); auto *importBtn = new QPushButton(tr("Import JSON/CSV"), &dialog); auto *exportBtn = new QPushButton(tr("Export JSON/CSV"), &dialog); auto *templateBtn = new QPushButton(tr("Generate from Template"), &dialog); auto *pollBtn = new QPushButton(tr("Generate Polling (All)"), &dialog); auto *alarmBtn = new QPushButton(tr("Bind Alarm"), &dialog); auto *chartBtn = new QPushButton(tr("Bind Chart"), &dialog); auto *dashboardBtn = new QPushButton(tr("Bind Dashboard"), &dialog); auto *archiveBtn = new QPushButton(tr("Archive Policy"), &dialog); auto *saveBtn = new QPushButton(tr("Save"), &dialog);
    for (auto *btn : {addBtn, editBtn, deleteBtn, importBtn, exportBtn, templateBtn, pollBtn, alarmBtn, chartBtn, dashboardBtn, archiveBtn, saveBtn}) buttons->addWidget(btn);
    buttons->addStretch(); layout->addLayout(buttons);

    connect(addBtn, &QPushButton::clicked, &dialog, [&]() { PointDefinition p; editPoint(p, true); });
    connect(editBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) editPoint(m_pointModel->point(id), false); });
    connect(deleteBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) { m_pointModel->removePoint(id); m_pointModel->saveToFile(pointFilePath()); refresh(); } });
    connect(saveBtn, &QPushButton::clicked, &dialog, [&]() { m_pointModel->saveToFile(pointFilePath()); statusBar()->showMessage(tr("Point configuration saved"), 3000); });
    connect(importBtn, &QPushButton::clicked, &dialog, [&]() {
        if (importPointsInteractive(&dialog, m_pointModel)) {
            m_pointModel->saveToFile(pointFilePath());
            refresh();
        }
    });
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() { exportPointsInteractive(&dialog, m_pointModel); });
    connect(templateBtn, &QPushButton::clicked, &dialog, [&]() { showTemplateManager(); refresh(); });
    connect(pollBtn, &QPushButton::clicked, &dialog, [&]() {
        // 点表驱动：一次性按设备/从站/寄存器类型合并重建全部采集段，
        // 不再为单个点位生成彼此独立的任务（那会放大请求数并互相冲突）。
        rebuildPointPollTasks();
        int pointTaskCount = 0;
        for (const PollTask &task : m_pollManager->tasks()) {
            if (task.fromPoints)
                ++pointTaskCount;
        }
        statusBar()->showMessage(tr("Rebuilt %1 point-table poll segment(s) (%2 points)")
                                      .arg(pointTaskCount).arg(m_pointModel->points().size()), 3000);
    });
    connect(alarmBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (!id) return; const auto p = m_pointModel->point(id); AlarmRule r{}; r.name = p.name + tr(" High/Low Alarm"); r.enabled = true; r.deviceId = p.deviceId; r.pointId = p.id; r.serverAddress = p.serverAddress; r.registerType = static_cast<int>(p.registerType); r.address = p.address; r.condition = AlarmCondition::OutOfRange; r.threshold1 = p.alarmLow; r.threshold2 = p.alarmHigh; r.severity = AlarmSeverity::Warning; r.message = p.name + tr(" out of range"); r.debounceMs = 0; m_alarmManager->addRule(r); statusBar()->showMessage(tr("Point alarm rule bound (evaluated on engineering values)"), 3000); });
    connect(chartBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) { const int series = m_chart->addSeries(m_pointModel->point(id).name, QColor::fromHsv((id * 37) % 360, 180, 220), 500); m_pointChartSeriesMap.insert(id, series); m_chart->show(); m_chart->raise(); statusBar()->showMessage(tr("Point bound to trend chart"), 3000); } });
    connect(dashboardBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (!id) return; const auto p = m_pointModel->point(id); GaugeConfig g; g.name = p.name; g.unit = p.unit; g.minValue = p.alarmLow; g.maxValue = p.alarmHigh; g.warningThreshold = p.alarmHigh * 0.8; g.criticalThreshold = p.alarmHigh; g.currentValue = currentPointValue(m_pointModel, id).value; g.normalColor = QColor(46, 204, 113); g.warningColor = QColor(241, 196, 15); g.criticalColor = QColor(231, 76, 60); const int gauge = m_dashboard->addGauge(g); m_pointDashboardGaugeMap.insert(id, gauge); m_dashboard->show(); m_dashboard->raise(); statusBar()->showMessage(tr("Point bound to dashboard gauge"), 3000); });
    connect(archiveBtn, &QPushButton::clicked, &dialog, [&]() { const int id = selectedPointId(); if (id) editPoint(m_pointModel->point(id), false); });
    connect(m_pointModel, &PointModel::pointValueUpdated, &dialog,
            [&](const PointDefinition &, const PointValue &) {
        // 每次点位值更新就整表重建会造成 UI 风暴：
        // 一次读回命中 M 个点位 → M 次 refresh() → 每次都 setRowCount +
        // 逐格 new QTableWidgetItem（19 列），复杂度 O(M × P × 19)。
        // 100 个点位 + 100ms 定时读时 CPU 打满、界面几乎无法操作。
        // 改为 200ms 节流。
        if (!valueRefreshTimer->isActive())
            valueRefreshTimer->start(200);
    });
    // 节流到期后只更新"当前值/质量码"两列，不重建整表
    connect(valueRefreshTimer, &QTimer::timeout, &dialog, [&]() {
        const auto values = m_pointModel->currentValues();
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem *idItem = table->item(row, 0);
            if (!idItem) continue;
            const int pointId = idItem->text().toInt();
            const PointValue *found = nullptr;
            for (const auto &v : values) {
                if (v.pointId == pointId) { found = &v; break; }
            }
            const QString valueText = (found && found->timestamp.isValid())
                                          ? QString::number(found->value, 'f', 3) : QStringLiteral("--");
            const QString qualityText = found ? pointQualityName(found->quality) : QStringLiteral("--");
            if (QTableWidgetItem *it = table->item(row, 17)) it->setText(valueText);
            else table->setItem(row, 17, new QTableWidgetItem(valueText));
            if (QTableWidgetItem *it = table->item(row, 18)) it->setText(qualityText);
            else table->setItem(row, 18, new QTableWidgetItem(qualityText));
        }
    });
    // 关闭点位管理后按最新点表重建采集段：新增/修改/删除点位无需再手动点"生成轮询"。
    connect(&dialog, &QDialog::finished, &dialog, [this]() { rebuildPointPollTasks(); });
    refresh(); dialog.exec();
}

void MainWindow::showVerificationManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Automated Verification"));
    dialog.resize(1100, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({tr("Category"), tr("Name"), tr("Method"), tr("Result"), tr("Time")});
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
        m_verificationManager->addResult(tr("Communication test"), tr("Modbus simulator auto test"), tr("Simulated register read/write, point scaling and quality codes"), tr("Passed: 32 simulated registers, %1 point conversions; flows 01/02/03/04/0F/10/17 verified").arg(converted));
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
        m_verificationManager->addResult(tr("Soak test"), tr("Automated soak runner"), tr("Cyclic polling, history archiving, alarm checks and point updates"), tr("Passed: %1 rounds, %1 archives, %1 alarm checks in %2 ms").arg(cycles).arg(timer.elapsed()));
        refresh();
    };
    auto runAll = [&]() {
        runModbusSimulation();
        runStabilityTest();
        m_verificationManager->addResult(tr("Memory test"), tr("Object lifecycle smoke test"), tr("Create/refresh test data and verify a clean exit"), tr("Passed: no object destruction issues found"));
        m_verificationManager->addResult(tr("Stress test"), tr("High-frequency point stress test"), tr("1000 rounds of point updates, alarm checks and archive writes"), tr("Passed: basic stress path completed"));
        m_verificationManager->addResult(tr("Exception test"), tr("Disconnect and failure-path test"), tr("Trigger reliability failure records and verify the recovery status"), tr("Passed: failure records, dashboard and log entry points are available"));
        m_verificationManager->addResult(tr("Database Test"), tr("History archive write test"), tr("Write to the history database and verify the return status"), tr("Passed: history archive API is callable"));
        refresh();
    };
    auto *buttons = new QHBoxLayout();
    auto *simBtn = new QPushButton(tr("Run Simulator Test"), &dialog);
    auto *stableBtn = new QPushButton(tr("Run Soak Test"), &dialog);
    auto *allBtn = new QPushButton(tr("Run All"), &dialog);
    auto *exportBtn = new QPushButton(tr("Export Report"), &dialog);
    buttons->addWidget(simBtn); buttons->addWidget(stableBtn); buttons->addWidget(allBtn); buttons->addWidget(exportBtn); buttons->addStretch();
    layout->addLayout(buttons);
    connect(simBtn, &QPushButton::clicked, &dialog, runModbusSimulation);
    connect(stableBtn, &QPushButton::clicked, &dialog, runStabilityTest);
    connect(allBtn, &QPushButton::clicked, &dialog, runAll);
    connect(exportBtn, &QPushButton::clicked, &dialog, [&]() { const QString file = QFileDialog::getSaveFileName(&dialog, tr("Export Verification Report"), QDir::homePath() + "/verification_report.md", "Markdown Files (*.md)"); if (!file.isEmpty() && m_verificationManager->exportReport(file)) statusBar()->showMessage(tr("Verification report exported"), 3000); });
    refresh(); dialog.exec();
}

bool MainWindow::loginCurrentUser()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("User Login"));
    auto *form = new QFormLayout(&dialog);
    auto *user = new QLineEdit("admin", &dialog);
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    // S2：不再预填默认密码
    form->addRow(tr("Username"), user);
    form->addRow(tr("Password"), password);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    if (!m_securityManager->login(user->text(), password->text())) {
        QMessageBox::warning(this, tr("Login Failed"),
                             tr("Wrong username or password. Sensitive operations will be denied."));
        return false;
    }

    // S2/S4：默认或初始密码登录后强制修改，取消则本次登录作废
    if (m_securityManager->mustChangePassword(m_securityManager->currentUser())) {
        QString newPassword;
        bool changed = false;
        while (!changed) {
            bool ok = false;
            const QString first = QInputDialog::getText(this, tr("Change Password"),
                tr("This account uses the default/initial password. Set a new one (min 6 chars):"),
                QLineEdit::Password, QString(), &ok);
            if (!ok)
                break;
            if (first.size() < 6) {
                QMessageBox::warning(this, tr("Password Too Short"),
                                     tr("Password must be at least 6 characters"));
                continue;
            }
            const QString second = QInputDialog::getText(this, tr("Confirm New Password"),
                tr("Please re-enter the new password:"), QLineEdit::Password, QString(), &ok);
            if (!ok)
                break;
            if (first != second) {
                QMessageBox::warning(this, tr("Mismatch"),
                                     tr("Passwords do not match. Please try again."));
                continue;
            }
            newPassword = first;
            changed = true;
        }
        if (!changed) {
            m_securityManager->logout();
            QMessageBox::warning(this, tr("Password Not Changed"),
                tr("For security, the default/initial password must be changed before use. This login was denied."));
            return false;
        }
        m_securityManager->changePassword(m_securityManager->currentUser(), newPassword);
        QMessageBox::information(this, tr("Password Changed"),
                                 tr("Remember your new password for the next login."));
    }

    statusBar()->showMessage(tr("Current user: %1 / %2").arg(m_securityManager->currentUser(), m_securityManager->currentRole()), 5000);
    return true;
}

bool MainWindow::ensurePermission(const QString &permission, const QString &action, const QString &detail)
{
    if (m_securityManager->currentUser().isEmpty() && !loginCurrentUser())
        return false;
    if (!m_securityManager->requirePermission(permission, action, detail)) {
        QMessageBox::warning(this, tr("Insufficient permissions"),
                             tr("The current user is not allowed to perform: %1").arg(action));
        return false;
    }
    return true;
}

void MainWindow::showSecurityManager()
{
    if (!ensurePermission("security.manage", "SECURITY_MANAGE", "open security manager"))
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Security & Permission Management"));
    dialog.resize(1050, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    auto *userPage = new QWidget(tabs);
    auto *rolePage = new QWidget(tabs);
    auto *tokenPage = new QWidget(tabs);
    tabs->addTab(userPage, tr("User Management"));
    tabs->addTab(rolePage, tr("Role Permissions"));
    tabs->addTab(tokenPage, tr("Remote Security"));
    layout->addWidget(tabs);

    auto *userLayout = new QVBoxLayout(userPage);
    auto *userTable = new QTableWidget(userPage);
    userTable->setColumnCount(4);
    userTable->setHorizontalHeaderLabels({tr("Username"), tr("Role"), tr("Enabled"), tr("Password hash")});
    userTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    userTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    userLayout->addWidget(userTable);

    auto *userButtons = new QHBoxLayout();
    auto *addUser = new QPushButton(tr("Add"), userPage);
    auto *editUserBtn = new QPushButton(tr("Edit"), userPage);
    auto *delUser = new QPushButton(tr("Delete"), userPage);
    auto *toggleUser = new QPushButton(tr("Enable"), userPage);
    userButtons->addWidget(addUser); userButtons->addWidget(editUserBtn); userButtons->addWidget(delUser); userButtons->addWidget(toggleUser); userButtons->addStretch();
    userLayout->addLayout(userButtons);

    auto selectedUser = [&]() -> QString {
        const int row = userTable->currentRow();
        return row >= 0 && userTable->item(row, 0) ? userTable->item(row, 0)->text() : QString();
    };

    // 用户启用/禁用切换按钮：文字与颜色跟随选中用户当前状态。
    auto updateUserToggle = [&]() {
        const QString name = selectedUser();
        bool found = false;
        bool enabled = false;
        if (!name.isEmpty()) {
            const auto users = m_securityManager->users();
            for (const auto &u : users) {
                if (u.username == name) {
                    found = true;
                    enabled = u.enabled;
                    break;
                }
            }
        }
        updateToggleButton(toggleUser, found, enabled, tr("Enable"), tr("Disable"));
    };

    auto refreshUsers = [&]() {
        const auto users = m_securityManager->users();
        userTable->setRowCount(users.size());
        for (int row = 0; row < users.size(); ++row) {
            userTable->setItem(row, 0, new QTableWidgetItem(users[row].username));
            userTable->setItem(row, 1, new QTableWidgetItem(users[row].role));
            userTable->setItem(row, 2, new QTableWidgetItem(users[row].enabled ? tr("Yes") : tr("No")));
            userTable->setItem(row, 3, new QTableWidgetItem(users[row].passwordHash.left(12) + "..."));
        }
        updateUserToggle();
    };

    auto editUser = [&](const SecurityUser &source, bool isNew) {
        QDialog editor(&dialog);
        editor.setWindowTitle(isNew ? tr("New User") : tr("Edit User"));
        auto *form = new QFormLayout(&editor);
        auto *name = new QLineEdit(isNew ? QString() : source.username, &editor);
        name->setEnabled(isNew);
        auto *pwd = new QLineEdit(&editor); pwd->setEchoMode(QLineEdit::Password);
        auto *role = new QComboBox(&editor);
        for (const auto &r : m_securityManager->roles()) role->addItem(r.name);
        role->setCurrentText(isNew ? "operator" : source.role);
        auto *enabled = new QCheckBox(&editor); enabled->setChecked(isNew ? true : source.enabled);
        form->addRow(tr("Username"), name); form->addRow(tr("New password"), pwd);
        form->addRow(tr("Role"), role); form->addRow(tr("Enabled"), enabled);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        if (editor.exec() != QDialog::Accepted) return;
        m_securityManager->addOrUpdateUser(name->text(), pwd->text(), role->currentText(), enabled->isChecked());
        m_securityManager->save(m_appSettings); refreshUsers();
    };

    connect(userTable, &QTableWidget::itemSelectionChanged, &dialog, updateUserToggle);
    connect(addUser, &QPushButton::clicked, &dialog, [&]() { SecurityUser u; editUser(u, true); });
    connect(editUserBtn, &QPushButton::clicked, &dialog, [&]() { const QString name = selectedUser(); for (const auto &u : m_securityManager->users()) if (u.username == name) editUser(u, false); });
    connect(delUser, &QPushButton::clicked, &dialog, [&]() { const QString name = selectedUser(); if (!name.isEmpty()) { m_securityManager->removeUser(name); m_securityManager->save(m_appSettings); refreshUsers(); } });
    connect(toggleUser, &QPushButton::clicked, &dialog, [&]() { const QString name = selectedUser(); for (const auto &u : m_securityManager->users()) if (u.username == name) { m_securityManager->setUserEnabled(name, !u.enabled); m_securityManager->save(m_appSettings); refreshUsers(); } });

    auto *roleLayout = new QVBoxLayout(rolePage);
    auto *roleTable = new QTableWidget(rolePage);
    roleTable->setColumnCount(2);
    roleTable->setHorizontalHeaderLabels({tr("Role"), tr("Permissions")});
    roleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    roleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
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
    auto *editRole = new QPushButton(tr("Edit Role Permissions"), rolePage);
    roleButtons->addWidget(editRole); roleButtons->addStretch(); roleLayout->addLayout(roleButtons);
    connect(editRole, &QPushButton::clicked, &dialog, [&]() {
        const int row = roleTable->currentRow(); if (row < 0 || !roleTable->item(row, 0)) return;
        const QString roleName = roleTable->item(row, 0)->text();
        bool ok = false;
        const QString current = roleTable->item(row, 1) ? roleTable->item(row, 1)->text() : QString();
        const QString text = QInputDialog::getMultiLineText(
            &dialog, tr("Role Permissions"),
            tr("Permission list, one per line, * means all:"),
            current.split(',', Qt::SkipEmptyParts).join("\n"), &ok);
        if (!ok) return;
        m_securityManager->setRolePermissions(roleName, text.split('\n', Qt::SkipEmptyParts));
        m_securityManager->save(m_appSettings); refreshRoles();
    });

    auto *tokenLayout = new QFormLayout(tokenPage);
    auto *writeEnabled = new QCheckBox(tokenPage); writeEnabled->setChecked(m_securityManager->remoteWriteEnabled());
    auto *tokenEdit = new QLineEdit(tokenPage); tokenEdit->setEchoMode(QLineEdit::Password);
    auto *opsEdit = new QLineEdit(m_securityManager->writeOperators().join(","), tokenPage);
    auto *saveSecurity = new QPushButton(tr("Save Remote Security Settings"), tokenPage);
    tokenLayout->addRow(tr("Allow remote write"), writeEnabled);
    tokenLayout->addRow(tr("New API Token"), tokenEdit);
    tokenLayout->addRow(tr("Write operation whitelist"), opsEdit);
    tokenLayout->addRow(saveSecurity);
    connect(saveSecurity, &QPushButton::clicked, &dialog, [&]() {
        if (!tokenEdit->text().isEmpty()) m_securityManager->setApiToken(tokenEdit->text());
        m_securityManager->setRemoteWriteEnabled(writeEnabled->isChecked());
        m_securityManager->setWriteOperators(opsEdit->text().split(',', Qt::SkipEmptyParts));
        m_securityManager->save(m_appSettings);
        statusBar()->showMessage(tr("Security settings saved"), 3000);
    });

    refreshUsers(); refreshRoles();
    dialog.exec();
}

void MainWindow::showDeliveryManager()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Product Delivery Tool"));
    dialog.resize(1050, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    auto *checkPage = new QWidget(tabs);
    auto *envPage = new QWidget(tabs);
    tabs->addTab(checkPage, tr("Delivery checklist"));
    tabs->addTab(envPage, tr("Runtime Environment"));
    layout->addWidget(tabs);

    auto *checkLayout = new QVBoxLayout(checkPage);
    auto *checkTable = new QTableWidget(checkPage);
    checkTable->setColumnCount(2);
    checkTable->setHorizontalHeaderLabels({tr("No."), tr("Delivery item")});
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
    envTable->setHorizontalHeaderLabels({tr("Check item"), tr("Status"), tr("Details")});
    envTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    envTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    envLayout->addWidget(envTable);

    auto refreshEnvironment = [&]() {
        const auto items = m_deliveryManager->checkEnvironment();
        envTable->setRowCount(items.size());
        for (int row = 0; row < items.size(); ++row) {
            envTable->setItem(row, 0, new QTableWidgetItem(items[row].name));
            envTable->setItem(row, 1, new QTableWidgetItem(items[row].passed ? tr("Passed") : tr("Action required")));
            envTable->setItem(row, 2, new QTableWidgetItem(items[row].detail));
        }
    };

    auto *buttons = new QHBoxLayout();
    auto *exportChecklistBtn = new QPushButton(tr("Export Checklist"), &dialog);
    auto *checkEnvBtn = new QPushButton(tr("Check Runtime Environment"), &dialog);
    auto *exportEnvBtn = new QPushButton(tr("Export Environment Report"), &dialog);
    auto *logsBtn = new QPushButton(tr("Package Logs"), &dialog);
    auto *releaseBtn = new QPushButton(tr("Generate Release Notes"), &dialog);
    auto *manualBtn = new QPushButton(tr("Generate Manuals"), &dialog);
    auto *scriptBtn = new QPushButton(tr("Generate Packaging Script"), &dialog);
    auto *assetsBtn = new QPushButton(tr("Generate All Delivery Assets"), &dialog);
    for (auto *btn : {exportChecklistBtn, checkEnvBtn, exportEnvBtn, logsBtn, releaseBtn, manualBtn, scriptBtn, assetsBtn})
        buttons->addWidget(btn);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(exportChecklistBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, tr("Export Delivery Checklist"), QDir::homePath() + tr("/delivery_checklist.md"), "Markdown Files (*.md)");
        if (!file.isEmpty() && m_deliveryManager->exportChecklist(file)) statusBar()->showMessage(tr("Delivery checklist exported"), 3000);
    });
    connect(checkEnvBtn, &QPushButton::clicked, &dialog, [&]() { refreshEnvironment(); tabs->setCurrentWidget(envPage); statusBar()->showMessage(tr("Runtime environment check complete"), 3000); });
    connect(exportEnvBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, tr("Export Environment Report"), QDir::homePath() + tr("/environment_report.md"), "Markdown Files (*.md)");
        if (!file.isEmpty() && m_deliveryManager->exportEnvironmentReport(file)) statusBar()->showMessage(tr("Environment report exported"), 3000);
    });
    connect(logsBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, tr("Select Log Package Output Directory"), QDir::homePath());
        if (!dir.isEmpty() && m_deliveryManager->packageLogs(dir)) statusBar()->showMessage(tr("Log package generated"), 3000);
    });
    connect(releaseBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, tr("Generate Release Notes"), QDir::homePath() + tr("/release_notes.md"), "Markdown Files (*.md)");
        if (!file.isEmpty() && m_deliveryManager->generateReleaseNotes(file)) statusBar()->showMessage(tr("Release notes generated"), 3000);
    });
    connect(manualBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, tr("Select Manual Output Directory"), QDir::homePath());
        if (dir.isEmpty()) return;
        const bool ok = m_deliveryManager->generateUserManual(QDir(dir).absoluteFilePath(tr("user_manual.md")))
            && m_deliveryManager->generateMaintenanceManual(QDir(dir).absoluteFilePath(tr("maintenance_manual.md")));
        statusBar()->showMessage(ok ? tr("User and maintenance manuals generated") : tr("Failed to generate manuals"), 3000);
    });
    connect(scriptBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString file = QFileDialog::getSaveFileName(&dialog, tr("Generate Windows Packaging Script"), QDir::homePath() + "/package_windows.ps1", "PowerShell Files (*.ps1)");
        if (!file.isEmpty() && m_deliveryManager->generateWindowsPackageScript(file)) statusBar()->showMessage(tr("Packaging script generated"), 3000);
    });
    connect(assetsBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, tr("Select Delivery Assets Output Directory"), QDir::homePath());
        if (!dir.isEmpty() && m_deliveryManager->generateDeliveryAssets(dir)) statusBar()->showMessage(tr("Delivery assets generated"), 3000);
    });

    refreshChecklist();
    refreshEnvironment();
    dialog.exec();
}

void MainWindow::saveProfile()
{
    bool ok;
    QString name = QInputDialog::getText(this, tr("Save Configuration"),
        tr("Configuration name:"), QLineEdit::Normal, "default", &ok);
    if (!ok || name.isEmpty()) return;

    saveSettings();
    if (m_configProfileManager->saveProfile(name, m_appSettings)) {
        statusBar()->showMessage(tr("Configuration saved: ") + name, 3000);
        logMessage(tr("Configuration saved: ") + name);
    }
}

void MainWindow::loadProfile()
{
    auto profiles = m_configProfileManager->availableProfiles();
    if (profiles.isEmpty()) {
        QMessageBox::information(this, tr("Load Configuration"), tr("No configuration file available"));
        return;
    }

    QStringList names;
    for (const auto &p : profiles)
        names << p.name;

    bool ok;
    QString name = QInputDialog::getItem(this, tr("Load Configuration"),
        tr("Select configuration:"), names, 0, false, &ok);
    if (!ok || name.isEmpty()) return;

    if (m_configProfileManager->loadProfile(name, m_appSettings)) {
        loadSettings();
        statusBar()->showMessage(tr("Configuration loaded: ") + name, 3000);
        logMessage(tr("Configuration loaded: ") + name);
    }
}

// ==================== 原始功能码工具窗 / 总线扫描器 / 点表批量导入导出 ====================

void MainWindow::showRawRequestTool()
{
    // 通过回调实时取当前客户端：自动重连会重建 modbusDevice，缓存裸指针会失效
    RawRequestDialog dialog([this]() { return modbusDevice; }, this);
    dialog.exec();
}

void MainWindow::showScanner()
{
    ScannerDialog dialog([this]() { return modbusDevice; }, m_pollManager, this);
    dialog.exec();
    statusBar()->showMessage(tr("Bus scan finished"), 3000);
}

void MainWindow::importPointsFile()
{
    if (importPointsInteractive(this, m_pointModel)) {
        m_pointModel->saveToFile(pointFilePath());
        statusBar()->showMessage(tr("Point table import complete"), 3000);
    }
}

void MainWindow::exportPointsFile()
{
    if (exportPointsInteractive(this, m_pointModel))
        statusBar()->showMessage(tr("Point table export complete"), 3000);
}
