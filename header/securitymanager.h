#ifndef SECURITYMANAGER_H
#define SECURITYMANAGER_H

#include <QObject>
#include <QString>
#include <QSet>
#include <QStringList>
#include <QDateTime>
#include <QCryptographicHash>
#include <QSettings>
#include <QMap>

struct SecurityUser {
    QString username;
    QString passwordHash;
    QString passwordSalt;              // S3：每用户随机盐（空 = 旧版无盐哈希，登录成功后透明升级）
    QString role;
    bool enabled = true;
    bool mustChangePassword = false;   // true = 使用默认/初始密码，登录后强制修改
};

struct SecurityRole {
    QString name;
    QStringList permissions;
};

class SecurityManager : public QObject
{
    Q_OBJECT

public:
    explicit SecurityManager(QObject *parent = nullptr);

    void load(QSettings &settings);
    void save(QSettings &settings) const;

    void setApiToken(const QString &token);
    QString apiTokenHash() const;
    bool verifyApiToken(const QString &token) const;

    // 强制改密（S2/S4）：账号标记 mustChangePassword 时，登录后必须先修改密码
    bool mustChangePassword(const QString &username) const;
    void changePassword(const QString &username, const QString &newPassword);

    void setRemoteWriteEnabled(bool enabled);
    bool remoteWriteEnabled() const;

    bool isWriteAllowed(const QString &operatorName, const QString &reason) const;
    void setWriteOperators(const QStringList &operators);
    QStringList writeOperators() const;

    void audit(const QString &operatorName, const QString &action, const QString &detail) const;
    QString auditLogPath() const;

    bool login(const QString &username, const QString &password);
    void logout();
    QString currentUser() const;
    QString currentRole() const;
    bool hasPermission(const QString &permission) const;
    bool requirePermission(const QString &permission, const QString &action, const QString &detail) const;

    void addOrUpdateUser(const QString &username, const QString &password, const QString &role, bool enabled = true);
    void removeUser(const QString &username);
    void setUserEnabled(const QString &username, bool enabled);
    QVector<SecurityUser> users() const;
    QVector<SecurityRole> roles() const;
    void setRolePermissions(const QString &role, const QStringList &permissions);
    QStringList availablePermissions() const;

private:
    // 旧版裸 SHA256（仅用于旧数据兼容校验，不再直接生成新凭据）
    QString hashToken(const QString &token) const;
    void ensureDefaults();
    QString hashPassword(const QString &password) const;

    // S3：加盐哈希（SHA256("v1:" + salt + ":" + secret)），防彩虹表预计算
    QString generateSalt() const;
    QString saltedHash(const QString &salt, const QString &secret) const;
    bool isLegacyHash(const QString &hash) const;
    bool verifySecret(const QString &salt, const QString &storedHash, const QString &secret) const;

    QString m_apiTokenHash;
    bool m_remoteWriteEnabled;
    QSet<QString> m_writeOperators;
    QMap<QString, SecurityUser> m_users;
    QMap<QString, SecurityRole> m_roles;
    QString m_currentUser;
};

#endif // SECURITYMANAGER_H
