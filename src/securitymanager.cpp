#include "securitymanager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDate>
#include <QDateTime>
#include <QUuid>

SecurityManager::SecurityManager(QObject *parent)
    : QObject(parent)
    , m_remoteWriteEnabled(false)
{
    m_writeOperators.insert(QStringLiteral("admin"));
}

void SecurityManager::load(QSettings &settings)
{
    // S1 修复：默认 Token 哈希置空。未配置 Token 时 verifyApiToken 一律拒绝，
    // 远程 API 在操作员显式设置 Token 前处于锁定状态（不再使用源码常量默认值）。
    m_apiTokenHash = settings.value("security/apiTokenHash").toString();
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
        user.passwordSalt = settings.value(QString("security/user/%1/passwordSalt").arg(username)).toString();
        user.role = settings.value(QString("security/user/%1/role").arg(username), "operator").toString();
        user.enabled = settings.value(QString("security/user/%1/enabled").arg(username), true).toBool();
        user.mustChangePassword = settings.value(QString("security/user/%1/mustChangePassword").arg(username), false).toBool();
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
        settings.setValue(QString("security/user/%1/passwordSalt").arg(user.username), user.passwordSalt);
        settings.setValue(QString("security/user/%1/role").arg(user.username), user.role);
        settings.setValue(QString("security/user/%1/enabled").arg(user.username), user.enabled);
        settings.setValue(QString("security/user/%1/mustChangePassword").arg(user.username), user.mustChangePassword);
    }
}

void SecurityManager::setApiToken(const QString &token)
{
    // S3：API Token 同样加盐（存储格式 "salt:hash"），不再使用裸 SHA256
    if (token.isEmpty()) {
        m_apiTokenHash.clear();
        return;
    }
    const QString salt = generateSalt();
    m_apiTokenHash = salt + QLatin1Char(':') + saltedHash(salt, token);
}

QString SecurityManager::apiTokenHash() const
{
    return m_apiTokenHash;
}

bool SecurityManager::verifyApiToken(const QString &token) const
{
    if (token.isEmpty() || m_apiTokenHash.isEmpty())
        return false;
    // S3：新格式 "salt:hash"
    const int sep = m_apiTokenHash.indexOf(QLatin1Char(':'));
    if (sep > 0) {
        const QString salt = m_apiTokenHash.left(sep);
        const QString stored = m_apiTokenHash.mid(sep + 1);
        return saltedHash(salt, token) == stored;
    }
    // 旧格式：裸 SHA256（向后兼容旧配置文件）
    return isLegacyHash(m_apiTokenHash) && hashToken(token) == m_apiTokenHash;
}

bool SecurityManager::mustChangePassword(const QString &username) const
{
    return m_users.value(username).mustChangePassword;
}

void SecurityManager::changePassword(const QString &username, const QString &newPassword)
{
    if (newPassword.isEmpty() || !m_users.contains(username))
        return;
    auto user = m_users.value(username);
    // S3：改密时生成新随机盐
    user.passwordSalt = generateSalt();
    user.passwordHash = saltedHash(user.passwordSalt, newPassword);
    user.mustChangePassword = false;
    m_users.insert(username, user);
    audit(username, QStringLiteral("PASSWORD_CHANGE"), QStringLiteral("success"));
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
    auto user = m_users.value(username);
    const bool ok = user.enabled && !user.username.isEmpty()
                    && verifySecret(user.passwordSalt, user.passwordHash, password);
    if (ok) {
        // S3：旧版无盐哈希在登录成功后透明升级为加盐哈希
        if (user.passwordSalt.isEmpty() && isLegacyHash(user.passwordHash)) {
            user.passwordSalt = generateSalt();
            user.passwordHash = saltedHash(user.passwordSalt, password);
            m_users.insert(username, user);
        }
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
    const bool isNew = !m_users.contains(username.trimmed());
    SecurityUser user = m_users.value(username.trimmed());
    user.username = username.trimmed();
    if (!password.isEmpty()) {
        // 显式设置密码：视为密码已变更，清除强制改密标记（S3：新随机盐）
        user.passwordSalt = generateSalt();
        user.passwordHash = saltedHash(user.passwordSalt, password);
        user.mustChangePassword = false;
    } else if (user.passwordHash.isEmpty()) {
        // S4：新建用户未提供密码 → 默认密码 123456 + 强制改密标记，
        // 默认密码仅能用于登录并触发修改流程，无法长期使用（S3：新随机盐）
        user.passwordSalt = generateSalt();
        user.passwordHash = saltedHash(user.passwordSalt, QStringLiteral("123456"));
        user.mustChangePassword = true;
    }
    user.role = role.trimmed().isEmpty() ? QStringLiteral("operator") : role.trimmed();
    user.enabled = enabled;
    m_users.insert(user.username, user);
    Q_UNUSED(isNew)
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
    if (m_users.isEmpty()) {
        addOrUpdateUser(QStringLiteral("admin"), QStringLiteral("admin123"), QStringLiteral("admin"), true);
        // S2：种子管理员使用公开默认密码，标记强制改密（登录后必须先修改）
        if (m_users.contains(QStringLiteral("admin")))
            m_users[QStringLiteral("admin")].mustChangePassword = true;
    }
}

QString SecurityManager::hashPassword(const QString &password) const
{
    return hashToken(QStringLiteral("pwd:") + password);
}

QString SecurityManager::hashToken(const QString &token) const
{
    return QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex());
}

// ---------------- S3：加盐哈希 ----------------

QString SecurityManager::generateSalt() const
{
    // 随机盐：128 位 UUID（32 个十六进制字符）+ 毫秒时间戳兜底
    return QUuid::createUuid().toString(QUuid::Id128)
           + QString::number(QDateTime::currentMSecsSinceEpoch(), 16);
}

QString SecurityManager::saltedHash(const QString &salt, const QString &secret) const
{
    return hashToken(QStringLiteral("v1:%1:%2").arg(salt, secret));
}

bool SecurityManager::isLegacyHash(const QString &hash) const
{
    if (hash.size() != 64)
        return false;
    for (const QChar &c : hash) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
              || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

bool SecurityManager::verifySecret(const QString &salt, const QString &storedHash, const QString &secret) const
{
    if (storedHash.isEmpty() || secret.isEmpty())
        return false;
    if (!salt.isEmpty())
        return saltedHash(salt, secret) == storedHash;
    // 兼容旧版无盐哈希（"pwd:" 前缀裸 SHA256）
    return isLegacyHash(storedHash) && hashToken(QStringLiteral("pwd:") + secret) == storedHash;
}
