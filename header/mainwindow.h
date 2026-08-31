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
class McpServer;
class AgentToolRegistry;

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
    void initMcpAgent();                       // AI/MCP：构建共用工具注册表与 MCP 服务器
    QModbusDataUnit readRequest() const;
    QModbusDataUnit writeRequest() const;

    void loadSettings();
    void saveSettings();
    void populateSerialPorts();
    void logMessage(const QString &line, int level = 1) const;
    void updateConnectionChip(bool connected);   // 顶部状态胶囊（红/绿）
    void applyLanguage(const QString &lang);     // 应用界面语言（zh_CN / en）

private slots:
    void on_connectButton_clicked();
    void onStateChanged(int state);
    void on_readButton_clicked();
    void readReady();
    void on_writeButton_clicked();
    void on_readWriteButton_clicked();
    void on_connectType_currentIndexChanged(int);
    void on_writeTable_currentIndexChanged(int);

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
    void toggleMcpServer();
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
    McpServer *m_mcpServer = nullptr;              // MCP 服务器（AI 能力出口之一）
    AgentToolRegistry *m_agentTools = nullptr;     // AI Agent / MCP 共用工具注册表
    QMap<int, int> m_pointChartSeriesMap;
    QMap<int, int> m_pointDashboardGaugeMap;
};

#endif // MAINWINDOW_H
