#ifndef DELIVERYMANAGER_H
#define DELIVERYMANAGER_H

#include <QObject>
#include <QStringList>
#include <QVector>

struct DeliveryEnvironmentItem {
    QString name;
    bool passed;
    QString detail;
};

class DeliveryManager : public QObject
{
    Q_OBJECT

public:
    explicit DeliveryManager(QObject *parent = nullptr);

    QStringList checklist() const;
    bool exportChecklist(const QString &filePath) const;
    QString versionText() const;

    QVector<DeliveryEnvironmentItem> checkEnvironment() const;
    bool exportEnvironmentReport(const QString &filePath) const;
    bool packageLogs(const QString &targetDir) const;
    bool generateReleaseNotes(const QString &filePath) const;
    bool generateUserManual(const QString &filePath) const;
    bool generateMaintenanceManual(const QString &filePath) const;
    bool generateWindowsPackageScript(const QString &filePath) const;
    bool generateDeliveryAssets(const QString &targetDir) const;

private:
    QStringList m_checklist;
};

#endif // DELIVERYMANAGER_H
