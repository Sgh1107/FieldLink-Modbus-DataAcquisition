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
        QModbusClient *client;
    };
    QVector<DeviceEntry> m_devices;
    int m_nextId;
};

#endif // DEVICEMANAGER_H
