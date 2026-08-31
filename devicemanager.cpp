#include "devicemanager.h"
#include <QSettings>
#include <QUrl>

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
    , m_nextId(1)
{
}

DeviceManager::~DeviceManager()
{
    for (auto &entry : m_devices) {
        if (entry.client) {
            entry.client->disconnectDevice();
            delete entry.client;
            entry.client = nullptr;
        }
    }
    m_devices.clear();
}

int DeviceManager::addDevice(const DeviceConfig &config)
{
    DeviceEntry entry;
    entry.config = config;
    entry.config.id = m_nextId++;
    entry.config.connected = false;
    entry.client = nullptr;
    m_devices.append(entry);
    return entry.config.id;
}

void DeviceManager::removeDevice(int deviceId)
{
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices[i].config.id == deviceId) {
            if (m_devices[i].client) {
                m_devices[i].client->disconnectDevice();
                delete m_devices[i].client;
            }
            m_devices.removeAt(i);
            break;
        }
    }
}

void DeviceManager::updateDevice(const DeviceConfig &config)
{
    for (auto &entry : m_devices) {
        if (entry.config.id == config.id) {
            bool wasConnected = entry.config.connected;
            entry.config = config;
            entry.config.connected = wasConnected;
            break;
        }
    }
}

DeviceConfig DeviceManager::deviceConfig(int deviceId) const
{
    for (const auto &entry : m_devices) {
        if (entry.config.id == deviceId)
            return entry.config;
    }
    return DeviceConfig();
}

QVector<DeviceConfig> DeviceManager::allDevices() const
{
    QVector<DeviceConfig> result;
    for (const auto &entry : m_devices)
        result.append(entry.config);
    return result;
}

bool DeviceManager::connectDevice(int deviceId)
{
    for (auto &entry : m_devices) {
        if (entry.config.id == deviceId) {
            entry.config.lastError.clear();
            if (entry.client) {
                entry.client->disconnectDevice();
                delete entry.client;
                entry.client = nullptr;
            }

            if (entry.config.isTcp) {
                entry.client = new QModbusTcpClient(this);
                QUrl url = QUrl::fromUserInput(entry.config.portOrAddress);
                entry.client->setConnectionParameter(QModbusDevice::NetworkPortParameter, url.port());
                entry.client->setConnectionParameter(QModbusDevice::NetworkAddressParameter, url.host());
            } else {
                entry.client = new QModbusRtuSerialClient(this);
                entry.client->setConnectionParameter(QModbusDevice::SerialPortNameParameter, entry.config.portOrAddress);
                entry.client->setConnectionParameter(QModbusDevice::SerialParityParameter, entry.config.parity);
                entry.client->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, entry.config.baud);
                entry.client->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, entry.config.dataBits);
                entry.client->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, entry.config.stopBits);
            }

            entry.client->setTimeout(entry.config.responseTime);
            entry.client->setNumberOfRetries(entry.config.numberOfRetries);

            connect(entry.client, &QModbusClient::stateChanged, this, [this, deviceId](int state) {
                for (auto &e : m_devices) {
                    if (e.config.id == deviceId) {
                        e.config.connected = (state == QModbusDevice::ConnectedState);
                        if (e.config.connected)
                            emit deviceConnected(deviceId);
                        else
                            emit deviceDisconnected(deviceId);
                        break;
                    }
                }
            });

            connect(entry.client, &QModbusClient::errorOccurred, this, [this, deviceId](QModbusDevice::Error) {
                for (auto &e : m_devices) {
                    if (e.config.id == deviceId && e.client) {
                        e.config.lastError = e.client->errorString();
                        emit deviceError(deviceId, e.config.lastError);
                        break;
                    }
                }
            });

            if (entry.client->connectDevice()) {
                return true;
            } else {
                entry.config.lastError = entry.client->errorString();
                emit deviceError(deviceId, entry.config.lastError);
                return false;
            }
        }
    }
    return false;
}

void DeviceManager::disconnectDevice(int deviceId)
{
    for (auto &entry : m_devices) {
        if (entry.config.id == deviceId && entry.client) {
            entry.client->disconnectDevice();
            break;
        }
    }
}

bool DeviceManager::isConnected(int deviceId) const
{
    for (const auto &entry : m_devices) {
        if (entry.config.id == deviceId)
            return entry.config.connected;
    }
    return false;
}

QModbusClient* DeviceManager::modbusClient(int deviceId) const
{
    for (const auto &entry : m_devices) {
        if (entry.config.id == deviceId)
            return entry.client;
    }
    return nullptr;
}

void DeviceManager::saveToSettings(QSettings &settings) const
{
    settings.beginWriteArray("devices");
    for (int i = 0; i < m_devices.size(); ++i) {
        settings.setArrayIndex(i);
        const auto &cfg = m_devices[i].config;
        settings.setValue("name", cfg.name);
        settings.setValue("isTcp", cfg.isTcp);
        settings.setValue("portOrAddress", cfg.portOrAddress);
        settings.setValue("serverAddress", cfg.serverAddress);
        settings.setValue("parity", cfg.parity);
        settings.setValue("baud", cfg.baud);
        settings.setValue("dataBits", cfg.dataBits);
        settings.setValue("stopBits", cfg.stopBits);
        settings.setValue("responseTime", cfg.responseTime);
        settings.setValue("numberOfRetries", cfg.numberOfRetries);
        settings.setValue("pollIntervalMs", cfg.pollIntervalMs);
        settings.setValue("archiveTag", cfg.archiveTag);
    }
    settings.endArray();
}

void DeviceManager::loadFromSettings(QSettings &settings)
{
    for (auto &entry : m_devices) {
        if (entry.client) {
            entry.client->disconnectDevice();
            delete entry.client;
            entry.client = nullptr;
        }
    }
    m_devices.clear();

    int size = settings.beginReadArray("devices");
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        DeviceConfig cfg;
        cfg.name = settings.value("name").toString();
        cfg.isTcp = settings.value("isTcp").toBool();
        cfg.portOrAddress = settings.value("portOrAddress").toString();
        cfg.serverAddress = settings.value("serverAddress").toInt();
        cfg.parity = settings.value("parity").toInt();
        cfg.baud = settings.value("baud").toInt();
        cfg.dataBits = settings.value("dataBits").toInt();
        cfg.stopBits = settings.value("stopBits").toInt();
        cfg.responseTime = settings.value("responseTime", 1000).toInt();
        cfg.numberOfRetries = settings.value("numberOfRetries", 3).toInt();
        cfg.pollIntervalMs = settings.value("pollIntervalMs", 1000).toInt();
        cfg.archiveTag = settings.value("archiveTag", cfg.name).toString();
        cfg.lastError.clear();
        cfg.connected = false;
        addDevice(cfg);
    }
    settings.endArray();
}
