#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QModbusDataUnit>
#include <QSettings>
#include <QString>
#include <QTranslator>
#include <QJsonObject>
#include <QMap>
#include <QHash>
#include <QSet>
#include <functional>

QT_BEGIN_NAMESPACE

class QModbusClient;
class QModbusReply;
class QListWidgetItem;

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
struct PointDefinition;
struct PointValue;

// 读取结果刷新模式随请求下发的属性名。
// true = 按地址原地刷新既有行（定时读取）；false = 追加新行（手动读取）。
// 不能用共享成员标志：标志在「发送时写、回包时读」，回包异步，
// 期间会被另一次请求改写，导致定时读与手动读并发时模式串扰。
static const char *const kReadInPlaceProperty = "fieldlink.readInPlace";

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
    void showRawRequestTool();                 // 原始功能码 / 报文发送工具窗
    void showScanner();                        // Modbus 总线扫描器
    void importPointsFile();                   // 点表批量导入（CSV/JSON）
    void exportPointsFile();                   // 点表批量导出（CSV/JSON）
    void showVerificationManager();
    void showDeliveryManager();
    void showSecurityManager();
    void showHistoryQuery();
    bool ensurePermission(const QString &permission, const QString &action, const QString &detail);
    bool loginCurrentUser();
    void saveProfile();
    void loadProfile();
    QJsonObject buildRemoteStatus() const;
    void publishMqttTelemetry(int deviceId, int serverAddress, int registerType, int startAddress,
                              const QVector<quint16> &values);
    void publishMqttStatus(bool connected);
    void askReconnectAfterRemoteClose();   // 对端主动断开：弹窗询问用户是否重连

private:
    // 远程读/写：异步执行，完成后通过 callback 回传结果。
    // 不使用嵌套事件循环（会造成 use-after-free 与 GUI 冻结），详见实现处注释。
    // 注意：这两个方法必须放在普通 private: 段而不是 private slots: 段 ——
    // moc 会为槽函数生成元对象调用代码，而 moc 无法正确解析
    // std::function<void(const QJsonObject&)> 这类参数类型（会把 const 放到错误位置），
    // 导致编译错误。它们本来就不是槽函数，不需要出现在元对象里。
    void executeRemoteRead(int serverAddress, int registerType, int startAddress, int count,
                           const std::function<void(const QJsonObject &)> &callback);
    void executeRemoteWrite(int serverAddress, int registerType, int startAddress,
                            const QVector<quint16> &values,
                            const std::function<void(const QJsonObject &)> &callback);

    // ---------- 多设备 / 点表驱动采集 ----------
    // deviceId 0 = 主连接；>0 = DeviceManager 中的设备（返回 nullptr 表示设备不存在）
    QModbusClient *clientForDevice(int deviceId) const;
    QString deviceDisplayName(int deviceId) const;
    // 按点表重建采集段：同设备/从站/寄存器类型下合并相邻地址，周期取点位最小值。
    // 只清理并重建 fromPoints=true 的任务，人工任务不受影响。
    void rebuildPointPollTasks();
    // 归档节流：archiveEnabled / archiveIntervalSec / archiveDeadband 在这里真正生效。
    void maybeArchivePoint(const PointDefinition &point, const PointValue &value);
    // 陈旧看门狗：轮询运行中，超过 3×采集周期（且 ≥5s）未更新的点位置 Timeout。
    void updateQualityWatchdog();

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
    int m_pendingReadRequests = 0;                 // 在途读请求计数（分段读时准确判断是否仍有请求未回）
    class QLabel *m_autoReadIndicator = nullptr;   // 定时读取常驻状态指示（状态栏右侧）
    QMap<int, int> m_pointChartSeriesMap;
    QMap<int, int> m_pointDashboardGaugeMap;
    // 点位归档节流状态（pointId -> 上次落库时间/值/质量）
    QHash<int, QDateTime> m_lastArchiveTime;
    QHash<int, double> m_lastArchiveValue;
    QHash<int, int> m_lastArchiveQuality;
    class QTimer *m_qualityWatchdog = nullptr;      // 陈旧检测定时器
    int m_lastModbusState = -1;        // 上一次 Modbus 连接状态（调试用）
    bool m_hadRealConnection = false;  // 本次连接周期内是否真正连上过（识别"连上后意外断线"）
    bool m_userDisconnecting = false;  // 用户主动断开标记（抑制对端断开弹窗）
    bool m_shuttingDown = false;       // 析构中标志：置位后所有回调立即返回（防止空指针解引用）
    // 「读取结果」列表的 地址 → 行 映射（原地刷新用）。
    // 线性查找是 O(值数 × 行数)，高频率大范围读取时列表查找成为瓶颈。
    QHash<int, QListWidgetItem *> m_readRowsByAddr;
};

#endif // MAINWINDOW_H
