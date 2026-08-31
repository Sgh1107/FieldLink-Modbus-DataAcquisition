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
    int id;
    QString name;
    bool isTcp;
    QString portOrAddress;
    int serverAddress;
    int parity;
    int baud;
    int dataBits;
    int stopBits;
    int responseTime;
    int numberOfRetries;
    bool connected;
    QString lastError;
    int pollIntervalMs;
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
