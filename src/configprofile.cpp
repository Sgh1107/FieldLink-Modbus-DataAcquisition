#include "configprofile.h"
#include <QFileInfo>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>

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
    // P2-5：档案名来自 UI 输入，必须先过滤（非法名返回空路径 → 直接失败）
    const QString filePath = profileFilePath(name);
    if (filePath.isEmpty()) {
        qWarning("ConfigProfileManager: 非法档案名，已拒绝保存: %s", qPrintable(name));
        return false;
    }

    QSettings profileSettings(filePath, QSettings::IniFormat);
    for (const QString &key : settings.allKeys()) {
        profileSettings.setValue(key, settings.value(key));
    }
    profileSettings.sync();

    // P2-6：只有真正写盘成功才算保存成功（只读目录/无权限时 status() != NoError）
    if (profileSettings.status() != QSettings::NoError) {
        qWarning("ConfigProfileManager: 档案写入失败: %s", qPrintable(filePath));
        return false;
    }
    if (!QFile::exists(filePath)) {
        qWarning("ConfigProfileManager: 档案文件未生成: %s", qPrintable(filePath));
        return false;
    }

    m_currentProfile = name.trimmed();
    emit profileSaved(m_currentProfile);
    return true;
}

bool ConfigProfileManager::loadProfile(const QString &name, QSettings &settings)
{
    const QString filePath = profileFilePath(name);
    if (filePath.isEmpty()) {
        qWarning("ConfigProfileManager: 非法档案名，已拒绝加载: %s", qPrintable(name));
        return false;
    }
    if (!QFile::exists(filePath))
        return false;

    QSettings profileSettings(filePath, QSettings::IniFormat);
    for (const QString &key : profileSettings.allKeys()) {
        settings.setValue(key, profileSettings.value(key));
    }
    settings.sync();

    // P2-6：读取/写入出错时不得报告成功
    if (profileSettings.status() != QSettings::NoError
        || settings.status() != QSettings::NoError) {
        qWarning("ConfigProfileManager: 档案读取失败: %s", qPrintable(filePath));
        return false;
    }

    // 注意：这里是「合并」语义而不是「替换」语义——
    // 档案中出现的键会覆盖目标 settings 中的同名键，但目标 settings 里档案未覆盖的键会保留原值。
    m_currentProfile = name.trimmed();
    emit profileLoaded(m_currentProfile);
    return true;
}

bool ConfigProfileManager::deleteProfile(const QString &name)
{
    const QString filePath = profileFilePath(name);
    if (filePath.isEmpty()) {
        qWarning("ConfigProfileManager: 非法档案名，已拒绝删除: %s", qPrintable(name));
        return false;
    }
    if (QFile::remove(filePath)) {
        emit profileDeleted(name.trimmed());
        return true;
    }
    return false;
}

bool ConfigProfileManager::renameProfile(const QString &oldName, const QString &newName)
{
    const QString oldPath = profileFilePath(oldName);
    const QString newPath = profileFilePath(newName);
    // 任一名字非法 → 拒绝（避免通过改名把文件写到档案目录之外）
    if (oldPath.isEmpty() || newPath.isEmpty()) {
        qWarning("ConfigProfileManager: 非法档案名，已拒绝重命名");
        return false;
    }
    return QFile::rename(oldPath, newPath);
}

bool ConfigProfileManager::profileExists(const QString &name) const
{
    const QString filePath = profileFilePath(name);
    if (filePath.isEmpty())
        return false;
    return QFile::exists(filePath);
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
    // P2-5：目录穿越防护——档案名直接来自 UI 输入框，必须过滤后才允许拼路径。
    // 规则：
    //   1) 去掉首尾空白后不得为空；
    //   2) 不得包含路径分隔符/通配符等非法字符 [\\/:*?"<>|]；
    //   3) 不得包含 ".."（相对路径穿越，如 "../../escaped"）；
    //   4) 不得包含控制字符。
    // 非法时返回空 QString，所有调用方必须据此返回失败。
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return QString();
    if (trimmed.contains(QStringLiteral("..")))
        return QString();

    static const QRegularExpression illegalChars(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (illegalChars.match(trimmed).hasMatch())
        return QString();

    for (const QChar &c : trimmed) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            return QString();
    }

    if (m_profileDir.isEmpty())
        return QString();

    // 用 QDir 拼接，避免手工拼 "/" 造成分隔符不一致
    return QDir(m_profileDir).absoluteFilePath(trimmed + QStringLiteral(".ini"));
}
