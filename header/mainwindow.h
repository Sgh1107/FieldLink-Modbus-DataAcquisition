#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QModbusDataUnit>
#include <QSettings>
#include <QString>
#include <QTranslator>
#include <QJsonObject>
#include <QMap>

QT_BEGIN_NAMESPACE

class QModbusClient;
class QModbusReply;

namespace Ui {
class MainWindow;
class SettingsDialog;
}

QT_END_NAMESPACE

class SettingsDialog;
class WriteRegisterModel;
class LogViewer;
class PollManager;
class DataExporter;
class RealTimeChart;
class DeviceManager;
class DeviceTemplateManager;
class ScriptEngine;
class RemoteServer;
class HistoryData;
class PluginManager;
class AlarmManager;
class BatchTaskManager;
class Dashboard;
class ConfigProfileManager;
class ReliabilityManager;
class SecurityManager;
class PointModel;
class VerificationManager;
class DeliveryManager;
class MqttClient;
class DeviceSimulatorPanel;

struct PollTask;
struct BatchTask;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void initActions();
    void initAdvancedFeatures();
    void initMenus();
    void initMqttSupport();                    // MQTT：初始化发布端客户端与遥测/报警挂钩
    void attemptModbusConnection(bool manual); // 执行一次 Modbus 连接动作（手动/自动重连共用；手动失败弹窗）
    QModbusDataUnit readRequest() const;
    QModbusDataUnit writeRequest() const;

    void loadSettings();
    void saveSettings();
    void populateSerialPorts();
    void logMessage(const QString &line, int level = 1) const;
    void updateConnectionChip(bool connected);   // 顶部状态胶囊（红/绿）
    void applyLanguage(const QString &lang);     // 应用界面语言（zh_CN / en）
    bool sendReadRequests(bool clearResult);     // 发起分段读取；clearResult=先清空结果列表
    void updateAutoReadIndicator(bool on);       // 定时读取可见状态指示（状态栏常驻标签）

private slots:
    void on_connectButton_clicked();
    void onStateChanged(int state);
    void on_readButton_clicked();
    void readReady();
    void on_writeButton_clicked();
    void on_readWriteButton_clicked();
    void on_connectType_currentIndexChanged(int);
    void on_writeTable_currentIndexChanged(int);
    void onAutoReadToggled(bool checked);      // 实时读取：定时轮询开关
    void onAutoReadTimeout();                  // 实时读取：定时到点执行一次读取

    void onPollRequest(const PollTask &task);
    void onBatchReadTask(const BatchTask &task);
    void onBatchWriteTask(const BatchTask &task);
    void showLogViewer();
    void showDashboard();
    void showChart();
    void exportData();
    void startStopPolling();
    void showBatchTaskDialog();
    void showAlarmConfig();
    void showDeviceManager();
    void showTemplateManager();
    void showScriptConsole();
    void toggleRemoteServer();
    void showMqttSettings();
    void showDeviceSimulator();
    void showPluginManager();
    void showPointManager();
    void showVerificationManager();
    void showDeliveryManager();
    void showSecurityManager();
    void showHistoryQuery();
    bool ensurePermission(const QString &permission, const QString &action, const QString &detail);
    bool loginCurrentUser();
    void saveProfile();
    void loadProfile();
    QJsonObject buildRemoteStatus() const;
    QJsonObject executeRemoteRead(int serverAddress, int registerType, int startAddress, int count);
    QJsonObject executeRemoteWrite(int serverAddress, int registerType, int startAddress, const QVector<quint16> &values);
    void publishMqttTelemetry(int serverAddress, int registerType, int startAddress, const QVector<quint16> &values);
    void publishMqttStatus(bool connected);
    void askReconnectAfterRemoteClose();   // 对端主动断开：弹窗询问用户是否重连

private:
    Ui::MainWindow *ui;
    QModbusReply *lastRequest;
    QModbusClient *modbusDevice;
    SettingsDialog *m_settingsDialog;
    WriteRegisterModel *writeModel;
    QSettings m_appSettings;
    QTranslator *m_translator = nullptr;

    LogViewer *m_logViewer;
    PollManager *m_pollManager;
    DataExporter *m_dataExporter;
    RealTimeChart *m_chart;
    DeviceManager *m_deviceManager;
    DeviceTemplateManager *m_templateManager;
    ScriptEngine *m_scriptEngine;
    RemoteServer *m_remoteServer;
    HistoryData *m_historyData;
    PluginManager *m_pluginManager;
    AlarmManager *m_alarmManager;
    BatchTaskManager *m_batchTaskManager;
    Dashboard *m_dashboard;
    ConfigProfileManager *m_configProfileManager;
    ReliabilityManager *m_reliabilityManager;
    SecurityManager *m_securityManager;
    PointModel *m_pointModel;
    VerificationManager *m_verificationManager;
    DeliveryManager *m_deliveryManager;
    MqttClient *m_mqttClient = nullptr;            // MQTT 发布端客户端
    DeviceSimulatorPanel *m_deviceSimulatorPanel = nullptr;   // 模拟设备面板
    class QTimer *m_autoReadTimer = nullptr;       // 实时读取定时轮询
    bool m_autoReadBusy = false;                   // 定时读取在途保护：上一 tick 未完成则跳过
    bool m_readUpdatesInPlace = false;             // true=定时读取按地址原地刷新行；false=手动读取追加
    class QLabel *m_autoReadIndicator = nullptr;   // 定时读取常驻状态指示（状态栏右侧）
    QMap<int, int> m_pointChartSeriesMap;
    QMap<int, int> m_pointDashboardGaugeMap;
    int m_lastModbusState = -1;        // 上一次 Modbus 连接状态（识别"曾连接后意外断线"）
    bool m_userDisconnecting = false;  // 用户主动断开标记（抑制对端断开弹窗）
};

#endif // MAINWINDOW_H
