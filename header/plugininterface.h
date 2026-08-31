#ifndef PLUGININTERFACE_H
#define PLUGININTERFACE_H

#include <QString>
#include <QVariant>
#include <QVector>
#include <QtGlobal>
#include <QtPlugin>

class PluginInterface
{
public:
    virtual ~PluginInterface() = default;

    virtual QString name() const = 0;
    virtual QString version() const = 0;
    virtual QString description() const = 0;
    virtual QString author() const = 0;

    virtual bool initialize() = 0;
    virtual void shutdown() = 0;

    virtual void onDataReceived(int serverAddress, int registerType,
                                 int startAddress, const QVector<quint16> &values) = 0;
    virtual void onConnectionStateChanged(bool connected) = 0;

    virtual QVariant getSetting(const QString &key) const = 0;
    virtual void setSetting(const QString &key, const QVariant &value) = 0;
};

#define PluginInterface_iid "com.modbus.PluginInterface/1.0"
Q_DECLARE_INTERFACE(PluginInterface, PluginInterface_iid)

#endif // PLUGININTERFACE_H
