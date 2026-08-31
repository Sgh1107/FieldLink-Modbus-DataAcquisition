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
    QString role;
    bool enabled = true;
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
    QString hashToken(const QString &token) const;
    void ensureDefaults();
    QString hashPassword(const QString &password) const;

    QString m_apiTokenHash;
    bool m_remoteWriteEnabled;
    QSet<QString> m_writeOperators;
    QMap<QString, SecurityUser> m_users;
    QMap<QString, SecurityRole> m_roles;
    QString m_currentUser;
};

#endif // SECURITYMANAGER_H
