#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QDir>
#include <QPluginLoader>
#include <QJsonObject>
#include "plugininterface.h"

struct PluginInfo {
    QString filePath;
    QString name;
    QString version;
    QString description;
    QString author;
    bool loaded = false;
    bool enabled = false;
    QString lastError;
    QJsonObject config;
    PluginInterface *instance = nullptr;   // 未加载时必须为空，析构里会用到
    QPluginLoader *loader = nullptr;
};

class PluginManager : public QObject
{
    Q_OBJECT

public:
    explicit PluginManager(QObject *parent = nullptr);
    ~PluginManager();

    void setPluginDirectory(const QString &dir);
    void scanPlugins();
    bool loadPlugin(int index);
    void unloadPlugin(int index);
    bool reloadPlugin(int index);
    void setPluginEnabled(int index, bool enabled);
    void setPluginConfig(int index, const QJsonObject &config);
    QJsonObject pluginConfig(int index) const;
    QString pluginLastError(int index) const;
    void loadAllPlugins();
    void unloadAllPlugins();

    QVector<PluginInfo> plugins() const;
    int pluginCount() const;

    void notifyDataReceived(int serverAddress, int registerType,
                            int startAddress, const QVector<quint16> &values);
    void notifyConnectionStateChanged(bool connected);

signals:
    void pluginLoaded(const QString &name);
    void pluginUnloaded(const QString &name);
    void pluginError(const QString &name, const QString &error);

private:
    QString configPath() const;
    void loadPluginConfigs();
    void savePluginConfigs() const;
    void setLastError(int index, const QString &error);

    QString m_pluginDir;
    QVector<PluginInfo> m_plugins;
};

#endif // PLUGINMANAGER_H
