#include "configprofile.h"
#include <QFileInfo>
#include <QDirIterator>
#include <QFile>

ConfigProfileManager::ConfigProfileManager(QObject *parent)
    : QObject(parent)
{
}

void ConfigProfileManager::setProfileDirectory(const QString &dir)
{
    m_profileDir = dir;
    QDir().mkpath(dir);
}

QString ConfigProfileManager::profileDirectory() const
{
    return m_profileDir;
}

QVector<ConfigProfile> ConfigProfileManager::availableProfiles() const
{
    QVector<ConfigProfile> profiles;
    QDir dir(m_profileDir);
    if (!dir.exists()) return profiles;

    QStringList filters;
    filters << "*.ini";
    for (const QFileInfo &fi : dir.entryInfoList(filters, QDir::Files, QDir::Time)) {
        ConfigProfile p;
        p.name = fi.baseName();
        p.filePath = fi.absoluteFilePath();
        p.lastModified = fi.lastModified();
        profiles.append(p);
    }
    return profiles;
}

bool ConfigProfileManager::saveProfile(const QString &name, const QSettings &settings)
{
    QString filePath = profileFilePath(name);
    QSettings profileSettings(filePath, QSettings::IniFormat);

    for (const QString &key : settings.allKeys()) {
        profileSettings.setValue(key, settings.value(key));
    }
    profileSettings.sync();

    m_currentProfile = name;
    emit profileSaved(name);
    return true;
}

bool ConfigProfileManager::loadProfile(const QString &name, QSettings &settings)
{
    QString filePath = profileFilePath(name);
    if (!QFile::exists(filePath)) return false;

    QSettings profileSettings(filePath, QSettings::IniFormat);
    for (const QString &key : profileSettings.allKeys()) {
        settings.setValue(key, profileSettings.value(key));
    }
    settings.sync();

    m_currentProfile = name;
    emit profileLoaded(name);
    return true;
}

bool ConfigProfileManager::deleteProfile(const QString &name)
{
    QString filePath = profileFilePath(name);
    if (QFile::remove(filePath)) {
        emit profileDeleted(name);
        return true;
    }
    return false;
}

bool ConfigProfileManager::renameProfile(const QString &oldName, const QString &newName)
{
    QString oldPath = profileFilePath(oldName);
    QString newPath = profileFilePath(newName);
    return QFile::rename(oldPath, newPath);
}

bool ConfigProfileManager::profileExists(const QString &name) const
{
    return QFile::exists(profileFilePath(name));
}

QString ConfigProfileManager::currentProfileName() const
{
    return m_currentProfile;
}

void ConfigProfileManager::setCurrentProfileName(const QString &name)
{
    m_currentProfile = name;
}

QString ConfigProfileManager::profileFilePath(const QString &name) const
{
    return m_profileDir + "/" + name + ".ini";
}
