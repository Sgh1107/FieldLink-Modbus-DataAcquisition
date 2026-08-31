#ifndef CONFIGPROFILE_H
#define CONFIGPROFILE_H

#include <QObject>
#include <QString>
#include <QSettings>
#include <QVector>
#include <QDir>
#include <QDateTime>

struct ConfigProfile {
    QString name;
    QString filePath;
    QString description;
    QDateTime lastModified;
};

class ConfigProfileManager : public QObject
{
    Q_OBJECT

public:
    explicit ConfigProfileManager(QObject *parent = nullptr);

    void setProfileDirectory(const QString &dir);
    QString profileDirectory() const;

    QVector<ConfigProfile> availableProfiles() const;
    bool saveProfile(const QString &name, const QSettings &settings);
    bool loadProfile(const QString &name, QSettings &settings);
    bool deleteProfile(const QString &name);
    bool renameProfile(const QString &oldName, const QString &newName);
    bool profileExists(const QString &name) const;

    QString currentProfileName() const;
    void setCurrentProfileName(const QString &name);

signals:
    void profileSaved(const QString &name);
    void profileLoaded(const QString &name);
    void profileDeleted(const QString &name);

private:
    QString profileFilePath(const QString &name) const;

    QString m_profileDir;
    QString m_currentProfile;
};

#endif // CONFIGPROFILE_H
