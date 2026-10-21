#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QSettings>
#include <QModbusClient>
#include <QModbusTcpClient>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtSerialBus/qmodbusrtuserialclient.h>
#else
// Qt 5 中该类名为 QModbusRtuSerialMaster，Qt 6.2 起才更名为 QModbusRtuSerialClient
#include <QtSerialBus/qmodbusrtuserialmaster.h>
using QModbusRtuSerialClient = QModbusRtuSerialMaster;
#endif

struct DeviceConfig {
    int id = 0;
    QString name;
    bool isTcp = true;
    QString portOrAddress;
    int serverAddress = 1;
    // 串口参数默认值与 UI 初始值一致（9600 8N1、无校验）
    int parity = 0;
    int baud = 9600;
    int dataBits = 8;
    int stopBits = 1;
    int responseTime = 1000;
    int numberOfRetries = 3;
    bool connected = false;
    QString lastError;
    int pollIntervalMs = 1000;
    QString archiveTag;
};

class DeviceManager : public QObject
{
    Q_OBJECT

public:
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager();

    int addDevice(const DeviceConfig &config);
    void removeDevice(int deviceId);
    void updateDevice(const DeviceConfig &config);
    DeviceConfig deviceConfig(int deviceId) const;
    QVector<DeviceConfig> allDevices() const;

    bool connectDevice(int deviceId);
    void disconnectDevice(int deviceId);
    bool isConnected(int deviceId) const;
    QModbusClient* modbusClient(int deviceId) const;

    void saveToSettings(QSettings &settings) const;
    void loadFromSettings(QSettings &settings);

signals:
    void deviceConnected(int deviceId);
    void deviceDisconnected(int deviceId);
    void deviceError(int deviceId, const QString &error);

private:
    struct DeviceEntry {
        DeviceConfig config;
        QModbusClient *client = nullptr;
    };

    // P2-3：加载时对缺键/越界值做归一（回落 DeviceConfig 结构体默认值）
    static void sanitizeConfig(DeviceConfig &cfg);
    // P2-11：判断影响「链路」的参数是否发生变化
    static bool connectionParamsChanged(const DeviceConfig &a, const DeviceConfig &b);
    // 按当前配置创建 client 并接好信号（不建立连接）
    void createClient(DeviceEntry &entry);
    // 断开并释放 client（释放前先把 entry.client 置空，使旧 client 的迟到信号被忽略）
    void destroyClient(DeviceEntry &entry);

    QVector<DeviceEntry> m_devices;
    int m_nextId;
};

#endif // DEVICEMANAGER_H
