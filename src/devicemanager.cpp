#include "devicemanager.h"
#include <QSettings>
#include <QUrl>
#include <QDebug>

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
    , m_nextId(1)
{
}

DeviceManager::~DeviceManager()
{
    for (auto &entry : m_devices)
        destroyClient(entry);
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
            destroyClient(m_devices[i]);
            m_devices.removeAt(i);
            break;
        }
    }
}

void DeviceManager::updateDevice(const DeviceConfig &config)
{
    for (auto &entry : m_devices) {
        if (entry.config.id != config.id)
            continue;

        const bool wasConnected = entry.config.connected;
        // P2-11：连接相关参数变化时必须重建 client，否则界面显示 B、实际链路仍是 A
        const bool connChanged = connectionParamsChanged(entry.config, config);

        entry.config = config;
        entry.config.connected = wasConnected;

        if (connChanged && entry.client) {
            destroyClient(entry);
            entry.config.connected = false;
            createClient(entry);
            if (wasConnected) {
                // 之前处于连接态 → 用新参数重新建立连接
                if (!entry.client->connectDevice()) {
                    entry.config.lastError = entry.client->errorString();
                    emit deviceError(config.id, entry.config.lastError);
                }
            }
        }
        break;
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
        if (entry.config.id != deviceId)
            continue;

        entry.config.lastError.clear();
        // 释放旧 client（先断开再销毁）；旧 client 的 stateChanged 会因
        // entry.client 已被置空而被 lambda 忽略，避免「刚连上又被判为断开」的抖动
        destroyClient(entry);
        entry.config.connected = false;

        createClient(entry);

        if (entry.client->connectDevice())
            return true;

        entry.config.lastError = entry.client->errorString();
        emit deviceError(deviceId, entry.config.lastError);
        return false;
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
    // P2-4：非原子加载的修复——先整体解析到临时容器，全部成功后再整体替换 m_devices。
    // 这样即使中途出错，也不会留下「只剩一半设备」的残缺状态
    //（析构时 saveToSettings 会把残缺状态写回 ini，导致原本完好的配置被永久截断）。
    QVector<DeviceConfig> parsed;
    const int size = settings.beginReadArray("devices");
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        DeviceConfig cfg;
        // P2-3：缺键时使用与 DeviceConfig 结构体一致的默认值，
        // 不能用 toInt()/toBool() 的隐式默认值（0/false）——那会把 TCP 设备
        // 静默变成串口设备、串口参数全为 0，连接必然失败且现象与真实原因不符。
        cfg.name = settings.value("name").toString();
        cfg.isTcp = settings.value("isTcp", cfg.isTcp).toBool();
        cfg.portOrAddress = settings.value("portOrAddress").toString();
        cfg.serverAddress = settings.value("serverAddress", cfg.serverAddress).toInt();
        cfg.parity = settings.value("parity", cfg.parity).toInt();
        cfg.baud = settings.value("baud", cfg.baud).toInt();
        cfg.dataBits = settings.value("dataBits", cfg.dataBits).toInt();
        cfg.stopBits = settings.value("stopBits", cfg.stopBits).toInt();
        cfg.responseTime = settings.value("responseTime", cfg.responseTime).toInt();
        cfg.numberOfRetries = settings.value("numberOfRetries", cfg.numberOfRetries).toInt();
        cfg.pollIntervalMs = settings.value("pollIntervalMs", cfg.pollIntervalMs).toInt();
        cfg.archiveTag = settings.value("archiveTag", cfg.name).toString();
        cfg.lastError.clear();
        cfg.connected = false;
        sanitizeConfig(cfg);
        parsed.append(cfg);
    }
    settings.endArray();

    // 解析全部完成 → 整体替换
    for (auto &entry : m_devices)
        destroyClient(entry);
    m_devices.clear();

    m_nextId = 1;
    for (const DeviceConfig &cfg : parsed)
        addDevice(cfg);
}

// ---------------------------------------------------------------------
// 内部辅助
// ---------------------------------------------------------------------
void DeviceManager::sanitizeConfig(DeviceConfig &cfg)
{
    // 波特率：标准档位 1200..115200 均合法；自定义波特率放宽到 [1200, 921600]
    // （低于 1200 的 300/600 等老档位在现代 USB 转串口链路上基本不可用，
    //   统一按非法处理并回落到 9600，避免连接失败时现象与原因不符）
    if (cfg.baud < 1200 || cfg.baud > 921600)
        cfg.baud = 9600;

    // 数据位：Modbus RTU 支持 5..8
    if (cfg.dataBits < 5 || cfg.dataBits > 8)
        cfg.dataBits = 8;

    // 停止位：1 / 1.5 / 2（枚举值 1..3）
    if (cfg.stopBits < 1 || cfg.stopBits > 3)
        cfg.stopBits = 1;

    // 从站地址：Modbus 标准范围 1..247（0 为广播，不作为轮询目标）
    if (cfg.serverAddress < 1 || cfg.serverAddress > 247)
        cfg.serverAddress = 1;

    // 轮询间隔：最小 100ms，避免把设备打爆
    if (cfg.pollIntervalMs < 100)
        cfg.pollIntervalMs = 1000;

    // 响应超时：1ms..60000ms
    if (cfg.responseTime < 1 || cfg.responseTime > 60000)
        cfg.responseTime = 1000;

    // 重试次数：不允许负数
    if (cfg.numberOfRetries < 0)
        cfg.numberOfRetries = 3;

    // 校验位：QSerialPort 枚举 0(No) 2(Even) 3(Odd) 4(Space) 5(Mark)，1 非法
    if (cfg.parity < 0 || cfg.parity > 5)
        cfg.parity = 0;
}

bool DeviceManager::connectionParamsChanged(const DeviceConfig &a, const DeviceConfig &b)
{
    return a.portOrAddress != b.portOrAddress
        || a.isTcp != b.isTcp
        || a.baud != b.baud
        || a.parity != b.parity
        || a.dataBits != b.dataBits
        || a.stopBits != b.stopBits
        || a.responseTime != b.responseTime
        || a.numberOfRetries != b.numberOfRetries;
}

void DeviceManager::createClient(DeviceEntry &entry)
{
    const int deviceId = entry.config.id;
    QModbusClient *client = nullptr;

    if (entry.config.isTcp) {
        auto *tcp = new QModbusTcpClient(this);
        const QUrl url = QUrl::fromUserInput(entry.config.portOrAddress);
        tcp->setConnectionParameter(QModbusDevice::NetworkPortParameter, url.port());
        tcp->setConnectionParameter(QModbusDevice::NetworkAddressParameter, url.host());
        client = tcp;
    } else {
        auto *rtu = new QModbusRtuSerialClient(this);
        rtu->setConnectionParameter(QModbusDevice::SerialPortNameParameter, entry.config.portOrAddress);
        rtu->setConnectionParameter(QModbusDevice::SerialParityParameter, entry.config.parity);
        rtu->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, entry.config.baud);
        rtu->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, entry.config.dataBits);
        rtu->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, entry.config.stopBits);
        client = rtu;
    }

    client->setTimeout(entry.config.responseTime);
    client->setNumberOfRetries(entry.config.numberOfRetries);
    entry.client = client;

    // P2-12：client 即信号发送者（sender()）。两个 lambda 都统一比对
    // "当前 entry 的 client 是否就是发出信号的 client"，不一致一律忽略——
    // 否则旧 client 在被替换/销毁时发出的 stateChanged 会把设备误判为断开，
    // 造成「刚连上又被判为断开」的抖动。
    connect(client, &QModbusClient::stateChanged, this, [this, deviceId, client](int state) {
        for (auto &e : m_devices) {
            if (e.config.id != deviceId || e.client != client)
                continue;   // 不是当前 client 的信号 → 忽略
            const bool nowConnected = (state == QModbusDevice::ConnectedState);
            e.config.connected = nowConnected;
            if (nowConnected)
                emit deviceConnected(deviceId);
            else if (state == QModbusDevice::UnconnectedState)
                // 只在真正的「已断开」状态通知，中间态（连接中/关闭中）不通知，
                // 避免连接重建时出现「刚连上又被判为断开」的抖动
                emit deviceDisconnected(deviceId);
            break;
        }
    });

    connect(client, &QModbusClient::errorOccurred, this, [this, deviceId, client](QModbusDevice::Error) {
        for (auto &e : m_devices) {
            if (e.config.id != deviceId || e.client != client)
                continue;   // 不是当前 client 的信号 → 忽略
            e.config.lastError = client->errorString();
            emit deviceError(deviceId, e.config.lastError);
            break;
        }
    });
}

void DeviceManager::destroyClient(DeviceEntry &entry)
{
    QModbusClient *client = entry.client;
    // 先置空：断开/销毁旧 client 时发出的 stateChanged / errorOccurred
    // 会因 e.client != client 而被忽略
    entry.client = nullptr;
    if (!client)
        return;
    client->disconnectDevice();
    delete client;
}
