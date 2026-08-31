#include "pluginmanager.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>

PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
{
}

PluginManager::~PluginManager()
{
    unloadAllPlugins();
    for (auto &info : m_plugins) {
        delete info.loader;
        info.loader = nullptr;
    }
    m_plugins.clear();
}

void PluginManager::setPluginDirectory(const QString &dir)
{
    m_pluginDir = dir;
}

QString PluginManager::configPath() const
{
    return QDir(m_pluginDir).absoluteFilePath(QStringLiteral("plugin_config.json"));
}

void PluginManager::loadPluginConfigs()
{
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;
    const QJsonObject root = doc.object();
    for (auto &info : m_plugins) {
        const QJsonObject obj = root.value(info.filePath).toObject();
        if (!obj.isEmpty()) {
            info.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
            info.config = obj.value(QStringLiteral("config")).toObject();
        }
    }
}

void PluginManager::savePluginConfigs() const
{
    QDir().mkpath(m_pluginDir);
    QJsonObject root;
    for (const auto &info : m_plugins) {
        QJsonObject obj;
        obj[QStringLiteral("enabled")] = info.enabled;
        obj[QStringLiteral("config")] = info.config;
        root[info.filePath] = obj;
    }
    QFile file(configPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void PluginManager::setLastError(int index, const QString &error)
{
    if (index < 0 || index >= m_plugins.size())
        return;
    m_plugins[index].lastError = error;
    emit pluginError(m_plugins[index].name, error);
}

void PluginManager::scanPlugins()
{
    unloadAllPlugins();
    for (auto &info : m_plugins) {
        delete info.loader;
        info.loader = nullptr;
    }
    m_plugins.clear();

    QDir dir(m_pluginDir);
    if (!dir.exists()) return;

    QStringList filters;
#ifdef Q_OS_WIN
    filters << "*.dll";
#elif defined(Q_OS_LINUX)
    filters << "*.so";
#elif defined(Q_OS_MAC)
    filters << "*.dylib";
#endif

    for (const QString &fileName : dir.entryList(filters, QDir::Files)) {
        QString filePath = dir.absoluteFilePath(fileName);
        QPluginLoader *loader = new QPluginLoader(filePath, this);

        QJsonObject metaData = loader->metaData().value("MetaData").toObject();

        PluginInfo info;
        info.filePath = filePath;
        info.name = QFileInfo(fileName).baseName();
        info.version = metaData.value("version").toString("1.0");
        info.description = metaData.value("description").toString();
        info.author = metaData.value("author").toString();
        info.loaded = false;
        info.enabled = true;
        info.lastError.clear();
        info.config = QJsonObject();
        info.instance = nullptr;
        info.loader = loader;

        m_plugins.append(info);
    }
    loadPluginConfigs();
}

bool PluginManager::loadPlugin(int index)
{
    if (index < 0 || index >= m_plugins.size())
        return false;

    auto &info = m_plugins[index];
    if (!info.enabled) {
        setLastError(index, QStringLiteral("Plugin disabled"));
        return false;
    }
    if (info.loaded) return true;

    if (!info.loader->load()) {
        setLastError(index, info.loader->errorString());
        return false;
    }

    QObject *obj = info.loader->instance();
    info.instance = qobject_cast<PluginInterface*>(obj);
    if (!info.instance) {
        info.loader->unload();
        setLastError(index, QStringLiteral("Invalid plugin interface"));
        return false;
    }

    for (auto it = info.config.constBegin(); it != info.config.constEnd(); ++it)
        info.instance->setSetting(it.key(), it.value().toVariant());

    if (!info.instance->initialize()) {
        info.loader->unload();
        info.instance = nullptr;
        setLastError(index, QStringLiteral("Plugin initialization failed"));
        return false;
    }

    info.loaded = true;
    info.name = info.instance->name();
    info.version = info.instance->version();
    info.description = info.instance->description();
    info.author = info.instance->author();
    info.lastError.clear();

    emit pluginLoaded(info.name);
    return true;
}

void PluginManager::unloadPlugin(int index)
{
    if (index < 0 || index >= m_plugins.size())
        return;

    auto &info = m_plugins[index];
    if (!info.loaded) return;

    if (info.instance) {
        info.instance->shutdown();
        info.instance = nullptr;
    }

    info.loader->unload();
    info.loaded = false;
    emit pluginUnloaded(info.name);
}

bool PluginManager::reloadPlugin(int index)
{
    unloadPlugin(index);
    return loadPlugin(index);
}

void PluginManager::setPluginEnabled(int index, bool enabled)
{
    if (index < 0 || index >= m_plugins.size())
        return;
    m_plugins[index].enabled = enabled;
    if (!enabled)
        unloadPlugin(index);
    savePluginConfigs();
}

void PluginManager::setPluginConfig(int index, const QJsonObject &config)
{
    if (index < 0 || index >= m_plugins.size())
        return;
    m_plugins[index].config = config;
    if (m_plugins[index].loaded && m_plugins[index].instance) {
        for (auto it = config.constBegin(); it != config.constEnd(); ++it)
            m_plugins[index].instance->setSetting(it.key(), it.value().toVariant());
    }
    savePluginConfigs();
}

QJsonObject PluginManager::pluginConfig(int index) const
{
    if (index < 0 || index >= m_plugins.size())
        return QJsonObject();
    return m_plugins[index].config;
}

QString PluginManager::pluginLastError(int index) const
{
    if (index < 0 || index >= m_plugins.size())
        return QString();
    return m_plugins[index].lastError;
}

void PluginManager::loadAllPlugins()
{
    for (int i = 0; i < m_plugins.size(); ++i)
        loadPlugin(i);
}

void PluginManager::unloadAllPlugins()
{
    for (int i = 0; i < m_plugins.size(); ++i)
        unloadPlugin(i);
}

QVector<PluginInfo> PluginManager::plugins() const
{
    return m_plugins;
}

int PluginManager::pluginCount() const
{
    return m_plugins.size();
}

void PluginManager::notifyDataReceived(int serverAddress, int registerType,
                                        int startAddress, const QVector<quint16> &values)
{
    for (const auto &info : m_plugins) {
        if (info.loaded && info.instance && info.enabled) {
            info.instance->onDataReceived(serverAddress, registerType, startAddress, values);
        }
    }
}

void PluginManager::notifyConnectionStateChanged(bool connected)
{
    for (const auto &info : m_plugins) {
        if (info.loaded && info.instance && info.enabled) {
            info.instance->onConnectionStateChanged(connected);
        }
    }
}
