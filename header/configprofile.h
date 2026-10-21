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
    // 档案名非法（空、含 [\\/:*?"<>|]、含 ".."、含控制字符）时一律返回 false，
    // 且不会在档案目录之外创建/删除任何文件（P2-5 目录穿越防护）。
    bool saveProfile(const QString &name, const QSettings &settings);
    // 合并语义（不是替换）：档案中的键覆盖目标 settings 的同名键，
    // 目标 settings 中档案未覆盖的键保持原值。读取/写入出错时返回 false（P2-6）。
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
