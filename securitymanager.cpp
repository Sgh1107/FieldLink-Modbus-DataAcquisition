#include "securitymanager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDate>

SecurityManager::SecurityManager(QObject *parent)
    : QObject(parent)
    , m_remoteWriteEnabled(false)
{
    m_writeOperators.insert(QStringLiteral("admin"));
}

void SecurityManager::load(QSettings &settings)
{
    m_apiTokenHash = settings.value("security/apiTokenHash", hashToken(QStringLiteral("modbus-admin"))).toString();
    m_remoteWriteEnabled = settings.value("security/remoteWriteEnabled", false).toBool();
    const QStringList ops = settings.value("security/writeOperators", QStringList() << "admin").toStringList();
    setWriteOperators(ops);

    m_roles.clear();
    const QStringList roleNames = settings.value("security/roles", QStringList()).toStringList();
    for (const QString &roleName : roleNames) {
        SecurityRole role;
        role.name = roleName;
        role.permissions = settings.value(QString("security/role/%1/permissions").arg(roleName)).toStringList();
        if (!role.name.trimmed().isEmpty())
            m_roles.insert(role.name, role);
    }

    m_users.clear();
    const QStringList userNames = settings.value("security/users", QStringList()).toStringList();
    for (const QString &username : userNames) {
        SecurityUser user;
        user.username = username;
        user.passwordHash = settings.value(QString("security/user/%1/passwordHash").arg(username)).toString();
        user.role = settings.value(QString("security/user/%1/role").arg(username), "operator").toString();
        user.enabled = settings.value(QString("security/user/%1/enabled").arg(username), true).toBool();
        if (!user.username.trimmed().isEmpty())
            m_users.insert(user.username, user);
    }
    ensureDefaults();
}

void SecurityManager::save(QSettings &settings) const
{
    settings.setValue("security/apiTokenHash", m_apiTokenHash);
    settings.setValue("security/remoteWriteEnabled", m_remoteWriteEnabled);
    settings.setValue("security/writeOperators", writeOperators());
    settings.setValue("security/roles", QStringList(m_roles.keys()));
    for (const auto &role : m_roles) {
        settings.setValue(QString("security/role/%1/permissions").arg(role.name), role.permissions);
    }
    settings.setValue("security/users", QStringList(m_users.keys()));
    for (const auto &user : m_users) {
        settings.setValue(QString("security/user/%1/passwordHash").arg(user.username), user.passwordHash);
        settings.setValue(QString("security/user/%1/role").arg(user.username), user.role);
        settings.setValue(QString("security/user/%1/enabled").arg(user.username), user.enabled);
    }
}

void SecurityManager::setApiToken(const QString &token)
{
    m_apiTokenHash = hashToken(token);
}

QString SecurityManager::apiTokenHash() const
{
    return m_apiTokenHash;
}

bool SecurityManager::verifyApiToken(const QString &token) const
{
    return !token.isEmpty() && hashToken(token) == m_apiTokenHash;
}

void SecurityManager::setRemoteWriteEnabled(bool enabled)
{
    m_remoteWriteEnabled = enabled;
}

bool SecurityManager::remoteWriteEnabled() const
{
    return m_remoteWriteEnabled;
}

bool SecurityManager::isWriteAllowed(const QString &operatorName, const QString &reason) const
{
    Q_UNUSED(reason)
    return m_remoteWriteEnabled && m_writeOperators.contains(operatorName);
}

void SecurityManager::setWriteOperators(const QStringList &operators)
{
    m_writeOperators.clear();
    for (const QString &op : operators) {
        if (!op.trimmed().isEmpty())
            m_writeOperators.insert(op.trimmed());
    }
}

QStringList SecurityManager::writeOperators() const
{
    QStringList result;
    for (const QString &op : m_writeOperators)
        result << op;
    return result;
}

void SecurityManager::audit(const QString &operatorName, const QString &action, const QString &detail) const
{
    const QString dir = QCoreApplication::applicationDirPath() + "/audit";
    QDir().mkpath(dir);
    QFile file(dir + "/" + QDate::currentDate().toString("yyyyMMdd") + ".audit.log");
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;

    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
        << " operator=" << operatorName
        << " action=" << action
        << " detail=" << detail << "\n";
}

QString SecurityManager::auditLogPath() const
{
    return QCoreApplication::applicationDirPath() + "/audit";
}

bool SecurityManager::login(const QString &username, const QString &password)
{
    ensureDefaults();
    const auto user = m_users.value(username);
    const bool ok = user.enabled && !user.username.isEmpty() && user.passwordHash == hashPassword(password);
    if (ok) {
        m_currentUser = username;
        audit(username, QStringLiteral("LOGIN"), QStringLiteral("success"));
    } else {
        audit(username.isEmpty() ? QStringLiteral("unknown") : username, QStringLiteral("LOGIN"), QStringLiteral("failed"));
    }
    return ok;
}

void SecurityManager::logout()
{
    if (!m_currentUser.isEmpty())
        audit(m_currentUser, QStringLiteral("LOGOUT"), QStringLiteral("success"));
    m_currentUser.clear();
}

QString SecurityManager::currentUser() const
{
    return m_currentUser;
}

QString SecurityManager::currentRole() const
{
    return m_users.value(m_currentUser).role;
}

bool SecurityManager::hasPermission(const QString &permission) const
{
    if (m_currentUser.isEmpty())
        return false;
    const auto user = m_users.value(m_currentUser);
    if (!user.enabled)
        return false;
    const auto role = m_roles.value(user.role);
    return role.permissions.contains(QStringLiteral("*")) || role.permissions.contains(permission);
}

bool SecurityManager::requirePermission(const QString &permission, const QString &action, const QString &detail) const
{
    const bool ok = hasPermission(permission);
    audit(m_currentUser.isEmpty() ? QStringLiteral("anonymous") : m_currentUser,
          action,
          QStringLiteral("%1 permission=%2 detail=%3").arg(ok ? "allowed" : "denied", permission, detail));
    return ok;
}

void SecurityManager::addOrUpdateUser(const QString &username, const QString &password, const QString &role, bool enabled)
{
    if (username.trimmed().isEmpty())
        return;
    SecurityUser user = m_users.value(username.trimmed());
    user.username = username.trimmed();
    if (!password.isEmpty())
        user.passwordHash = hashPassword(password);
    if (user.passwordHash.isEmpty())
        user.passwordHash = hashPassword(QStringLiteral("123456"));
    user.role = role.trimmed().isEmpty() ? QStringLiteral("operator") : role.trimmed();
    user.enabled = enabled;
    m_users.insert(user.username, user);
}

void SecurityManager::removeUser(const QString &username)
{
    if (username == QStringLiteral("admin"))
        return;
    m_users.remove(username);
}

void SecurityManager::setUserEnabled(const QString &username, bool enabled)
{
    if (!m_users.contains(username))
        return;
    auto user = m_users.value(username);
    user.enabled = enabled;
    m_users.insert(username, user);
}

QVector<SecurityUser> SecurityManager::users() const
{
    QVector<SecurityUser> result;
    for (const auto &user : m_users)
        result.append(user);
    return result;
}

QVector<SecurityRole> SecurityManager::roles() const
{
    QVector<SecurityRole> result;
    for (const auto &role : m_roles)
        result.append(role);
    return result;
}

void SecurityManager::setRolePermissions(const QString &roleName, const QStringList &permissions)
{
    if (roleName.trimmed().isEmpty())
        return;
    SecurityRole role;
    role.name = roleName.trimmed();
    role.permissions = permissions;
    m_roles.insert(role.name, role);
}

QStringList SecurityManager::availablePermissions() const
{
    return QStringList()
        << QStringLiteral("local.write")
        << QStringLiteral("batch.execute")
        << QStringLiteral("script.execute")
        << QStringLiteral("security.manage")
        << QStringLiteral("remote.write")
        << QStringLiteral("delivery.manage")
        << QStringLiteral("plugin.manage")
        << QStringLiteral("alarm.manage")
        << QStringLiteral("point.manage");
}

void SecurityManager::ensureDefaults()
{
    if (m_roles.isEmpty()) {
        SecurityRole adminRole; adminRole.name = QStringLiteral("admin"); adminRole.permissions = QStringList() << QStringLiteral("*");
        SecurityRole engineerRole; engineerRole.name = QStringLiteral("engineer"); engineerRole.permissions = QStringList() << QStringLiteral("local.write") << QStringLiteral("batch.execute") << QStringLiteral("script.execute") << QStringLiteral("alarm.manage") << QStringLiteral("point.manage") << QStringLiteral("delivery.manage");
        SecurityRole operatorRole; operatorRole.name = QStringLiteral("operator"); operatorRole.permissions = QStringList() << QStringLiteral("local.write") << QStringLiteral("alarm.manage") << QStringLiteral("point.manage");
        SecurityRole viewerRole; viewerRole.name = QStringLiteral("viewer");
        m_roles.insert(adminRole.name, adminRole);
        m_roles.insert(engineerRole.name, engineerRole);
        m_roles.insert(operatorRole.name, operatorRole);
        m_roles.insert(viewerRole.name, viewerRole);
    }
    if (m_users.isEmpty())
        addOrUpdateUser(QStringLiteral("admin"), QStringLiteral("admin123"), QStringLiteral("admin"), true);
}

QString SecurityManager::hashPassword(const QString &password) const
{
    return hashToken(QStringLiteral("pwd:") + password);
}

QString SecurityManager::hashToken(const QString &token) const
{
    return QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex());
}
