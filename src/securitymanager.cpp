#include "securitymanager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDate>
#include <QDateTime>
#include <QUuid>
#include <QStandardPaths>
#include <QHash>
#include <QDebug>

namespace {

// ---------------------------------------------------------------------
// PBKDF2-HMAC-SHA256（RFC 8018）
// 仅依赖 QtCore（QCryptographicHash），避免密码派生引入 QtNetwork 依赖。
// ---------------------------------------------------------------------
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    const int blockSize = 64;                    // SHA-256 分组长度
    QByteArray k = key;
    if (k.size() > blockSize)
        k = QCryptographicHash::hash(k, QCryptographicHash::Sha256);
    k = k.leftJustified(blockSize, '\0');

    QByteArray ipad(blockSize, '\0');
    QByteArray opad(blockSize, '\0');
    for (int i = 0; i < blockSize; ++i) {
        const unsigned char kb = static_cast<unsigned char>(k.at(i));
        ipad[i] = static_cast<char>(kb ^ 0x36);
        opad[i] = static_cast<char>(kb ^ 0x5c);
    }
    const QByteArray inner = QCryptographicHash::hash(ipad + data, QCryptographicHash::Sha256);
    return QCryptographicHash::hash(opad + inner, QCryptographicHash::Sha256);
}

QByteArray pbkdf2Sha256(const QByteArray &password, const QByteArray &salt,
                         quint64 iterations, int dkLen)
{
    QByteArray out;
    out.reserve(dkLen);
    quint32 block = 1;
    while (out.size() < dkLen) {
        QByteArray blockSalt = salt;
        blockSalt.append(static_cast<char>((block >> 24) & 0xFF));
        blockSalt.append(static_cast<char>((block >> 16) & 0xFF));
        blockSalt.append(static_cast<char>((block >> 8) & 0xFF));
        blockSalt.append(static_cast<char>(block & 0xFF));

        // U_1 = PRF(P, S || INT(i))；U_j = PRF(P, U_{j-1})；T_i = U_1 XOR ... XOR U_c
        QByteArray u = hmacSha256(password, blockSalt);
        QByteArray t = u;                       // T_i 初始为 U_1
        for (quint64 j = 1; j < iterations; ++j) {
            u = hmacSha256(password, u);        // U_{j+1}（原始值，不能被 XOR 破坏）
            for (int k = 0; k < u.size(); ++k)
                t[k] = static_cast<char>(t.at(k) ^ u.at(k));
        }
        out.append(t);
        ++block;
    }
    out.truncate(dkLen);
    return out;
}

// PBKDF2 迭代次数（>= 100000，兼顾安全强度与登录响应时间）
const quint64 kPbkdf2Iterations = 100000;
// 派生密钥长度（字节）
const int kDerivedKeyLength = 32;
// 单个审计日志文件大小上限（5MB），超过后轮转
const qint64 kAuditMaxFileSize = 5LL * 1024 * 1024;
// 连续登录失败达到该次数后锁定
const int kMaxLoginFailures = 5;
// 锁定时长（秒）
const int kLoginLockoutSeconds = 60;
// 内置管理员账号名（角色/启用状态受保护）
const char *kAdminUserName = "admin";

} // namespace

SecurityManager::SecurityManager(QObject *parent)
    : QObject(parent)
    , m_remoteWriteEnabled(false)
{
    m_writeOperators.insert(QStringLiteral("admin"));
}

// ---------------------------------------------------------------------
// 载入 / 保存
// ---------------------------------------------------------------------
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
        role.name = roleName.trimmed();
        role.permissions = settings.value(QString("security/role/%1/permissions").arg(roleName)).toStringList();
        if (!role.name.isEmpty())
            m_roles.insert(role.name, role);
    }

    m_users.clear();
    const QStringList userNames = settings.value("security/users", QStringList()).toStringList();
    for (const QString &rawName : userNames) {
        // P2-45：用户名归一化（trim）——避免 " admin " 与 "admin" 变成两个账号
        const QString username = rawName.trimmed();
        if (username.isEmpty())
            continue;

        // 存量配置的键可能带空格：优先按原始键读取，缺失时回退到归一化键
        const auto userKey = [&](const QString &field) -> QString {
            const QString rawKey = QString("security/user/%1/%2").arg(rawName, field);
            if (settings.contains(rawKey))
                return rawKey;
            return QString("security/user/%1/%2").arg(username, field);
        };

        SecurityUser user;
        user.username = username;
        user.passwordHash = settings.value(userKey(QStringLiteral("passwordHash"))).toString();
        user.passwordSalt = settings.value(userKey(QStringLiteral("passwordSalt"))).toString();
        user.role = settings.value(userKey(QStringLiteral("role")), QStringLiteral("operator")).toString().trimmed();
        if (user.role.isEmpty())
            user.role = QStringLiteral("operator");
        user.enabled = settings.value(userKey(QStringLiteral("enabled")), true).toBool();
        user.mustChangePassword = settings.value(userKey(QStringLiteral("mustChangePassword")), false).toBool();
        // key 与 user.username 都使用 trim 后的值
        m_users.insert(username, user);
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

// ---------------------------------------------------------------------
// API Token
// ---------------------------------------------------------------------
void SecurityManager::setApiToken(const QString &token)
{
    // S3：API Token 加盐（存储格式 "salt:hash"）；P2-38：改用 PBKDF2（"salt:v2:<hex>"）
    if (token.isEmpty()) {
        m_apiTokenHash.clear();
        return;
    }
    const QString salt = generateSalt();
    m_apiTokenHash = salt + QLatin1Char(':') + strongHash(salt, token);
}

QString SecurityManager::apiTokenHash() const
{
    return m_apiTokenHash;
}

bool SecurityManager::verifyApiToken(const QString &token) const
{
    if (token.isEmpty() || m_apiTokenHash.isEmpty())
        return false;
    // 存储格式 "salt:hash"（hash 自身为 "v2:<hex>" 或旧版 "v1" 裸 hex）
    const int sep = m_apiTokenHash.indexOf(QLatin1Char(':'));
    if (sep > 0) {
        const QString salt = m_apiTokenHash.left(sep);
        const QString stored = m_apiTokenHash.mid(sep + 1);
        if (stored.startsWith(QStringLiteral("v2:"))) {
            // 新格式：PBKDF2-HMAC-SHA256
            return constantTimeEquals(strongHash(salt, token), stored);
        }
        // 旧格式（向后兼容）：SHA256("v1:" + salt + ":" + secret)
        return constantTimeEquals(legacySaltedHash(salt, token), stored);
    }
    // 旧格式：裸 SHA256（向后兼容旧配置文件）
    return isLegacyHash(m_apiTokenHash) && constantTimeEquals(hashToken(token), m_apiTokenHash);
}

// ---------------------------------------------------------------------
// 口令
// ---------------------------------------------------------------------
bool SecurityManager::mustChangePassword(const QString &username) const
{
    return m_users.value(username.trimmed()).mustChangePassword;
}

void SecurityManager::changePassword(const QString &username, const QString &newPassword)
{
    const QString name = username.trimmed();
    if (newPassword.isEmpty() || !m_users.contains(name))
        return;
    auto user = m_users.value(name);
    // 改密时生成新随机盐 + PBKDF2 派生（P2-38）
    user.passwordSalt = generateSalt();
    user.passwordHash = strongHash(user.passwordSalt, newPassword);
    user.mustChangePassword = false;
    m_users.insert(name, user);
    audit(name, QStringLiteral("PASSWORD_CHANGE"), QStringLiteral("success"));
}

bool SecurityManager::passwordChangePending() const
{
    if (m_currentUser.isEmpty())
        return false;
    return m_users.value(m_currentUser).mustChangePassword;
}

void SecurityManager::setForcePasswordChangeGate(bool enabled)
{
    m_forcePasswordChangeGate = enabled;
}

bool SecurityManager::forcePasswordChangeGate() const
{
    return m_forcePasswordChangeGate;
}

// ---------------------------------------------------------------------
// 远程写
// ---------------------------------------------------------------------
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
    return m_remoteWriteEnabled && m_writeOperators.contains(operatorName.trimmed());
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

// ---------------------------------------------------------------------
// 审计日志
// ---------------------------------------------------------------------
void SecurityManager::audit(const QString &operatorName, const QString &action, const QString &detail) const
{
    const QString dir = auditLogPath();
    if (!QDir().mkpath(dir)) {
        qWarning() << "[SecurityManager] 审计目录不可写，审计日志将丢失:" << dir;
        return;
    }

    const QString dayTag = QDate::currentDate().toString("yyyyMMdd");
    const QString filePath = dir + QLatin1Char('/') + dayTag + QStringLiteral(".audit.log");

    // 轮转：单文件超过上限 → 归档为 "<日期>.1.audit.log"
    QFileInfo fi(filePath);
    if (fi.exists() && fi.size() > kAuditMaxFileSize) {
        const QString archived = dir + QLatin1Char('/') + dayTag + QStringLiteral(".1.audit.log");
        QFile::remove(archived);
        if (!QFile::rename(filePath, archived))
            qWarning() << "[SecurityManager] 审计日志轮转失败:" << filePath;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        // P2-40：写失败不再静默丢弃，至少输出告警
        qWarning() << "[SecurityManager] 审计日志写入失败:" << filePath << file.errorString();
        return;
    }

    // P2-42：转义控制字符，防止用户名/详情中的换行伪造任意审计行
    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
        << " operator=" << sanitizeAuditField(operatorName, true)
        << " action=" << sanitizeAuditField(action, true)
        << " detail=" << sanitizeAuditField(detail, false)
        << "\n";
    out.flush();
}

QString SecurityManager::auditLogPath() const
{
    // P2-40：审计目录改用用户可写位置（装到 Program Files 下 applicationDirPath 不可写）
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    // 未设置 organization/application 时 AppDataLocation 就是用户漫游目录根，
    // 追加一级子目录，避免把审计日志直接写进用户根目录
    if (QCoreApplication::organizationName().isEmpty())
        base += QLatin1String("/FieldLink");
    return base + QLatin1String("/audit");
}

// ---------------------------------------------------------------------
// 登录 / 登出
// ---------------------------------------------------------------------
bool SecurityManager::login(const QString &username, const QString &password)
{
    ensureDefaults();

    const QString name = username.trimmed();
    const QDateTime now = QDateTime::currentDateTime();

    // P2-40：失败锁定——锁定窗口内直接拒绝（不做阻塞 sleep，避免卡 UI）
    LoginFailureState &state = m_loginFailures[name];
    if (state.lockedUntil.isValid()) {
        if (now < state.lockedUntil) {
            audit(name.isEmpty() ? QStringLiteral("unknown") : name,
                  QStringLiteral("LOGIN"), QStringLiteral("locked"));
            return false;
        }
        // 锁定窗口已过 → 计数清零，重新开始
        state = LoginFailureState();
    }

    SecurityUser user = m_users.value(name);
    const bool ok = user.enabled && !user.username.isEmpty()
                    && verifySecret(user.passwordSalt, user.passwordHash, password);
    if (ok) {
        // 旧版无盐/旧版加盐哈希在登录成功后透明升级为 PBKDF2 哈希
        if (!user.passwordHash.startsWith(QStringLiteral("v2:"))) {
            user.passwordSalt = generateSalt();
            user.passwordHash = strongHash(user.passwordSalt, password);
            m_users.insert(name, user);
        }
        m_currentUser = name;
        m_loginFailures.remove(name);   // 登录成功 → 失败计数清零
        audit(name, QStringLiteral("LOGIN"), QStringLiteral("success"));
    } else {
        ++state.failures;
        if (state.failures >= kMaxLoginFailures)
            state.lockedUntil = now.addSecs(kLoginLockoutSeconds);
        audit(name.isEmpty() ? QStringLiteral("unknown") : name,
              QStringLiteral("LOGIN"), QStringLiteral("failed"));
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
    // P2-41：强制改密期间只放行改密操作，避免其它调用方绕过 UI 层的强制改密
    if (m_forcePasswordChangeGate && user.mustChangePassword)
        return permission == QStringLiteral("security.changePassword");
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

// ---------------------------------------------------------------------
// 用户管理
// ---------------------------------------------------------------------
void SecurityManager::addOrUpdateUser(const QString &username, const QString &password,
                                      const QString &role, bool enabled)
{
    const QString name = username.trimmed();
    if (name.isEmpty())
        return;
    const bool isAdmin = (name == QLatin1String(kAdminUserName));

    SecurityUser user = m_users.value(name);
    user.username = name;
    if (!password.isEmpty()) {
        // 显式设置密码：视为密码已变更，清除强制改密标记（PBKDF2 + 新随机盐）
        user.passwordSalt = generateSalt();
        user.passwordHash = strongHash(user.passwordSalt, password);
        user.mustChangePassword = false;
    } else if (user.passwordHash.isEmpty()) {
        // S4：新建用户未提供密码 → 默认密码 123456 + 强制改密标记
        user.passwordSalt = generateSalt();
        user.passwordHash = strongHash(user.passwordSalt, QStringLiteral("123456"));
        user.mustChangePassword = true;
    }

    if (isAdmin) {
        // P2-43：内置管理员的角色与启用状态不可被降级/禁用，否则会锁死唯一管理入口
        user.role = QStringLiteral("admin");
        user.enabled = true;
    } else {
        user.role = role.trimmed().isEmpty() ? QStringLiteral("operator") : role.trimmed();
        user.enabled = enabled;
    }
    m_users.insert(name, user);
}

void SecurityManager::removeUser(const QString &username)
{
    const QString name = username.trimmed();
    if (name == QLatin1String(kAdminUserName))
        return;
    m_users.remove(name);
}

void SecurityManager::setUserEnabled(const QString &username, bool enabled)
{
    const QString name = username.trimmed();
    if (!m_users.contains(name))
        return;
    if (name == QLatin1String(kAdminUserName)) {
        // P2-43：内置管理员必须保持启用
        if (!enabled)
            qWarning() << "[SecurityManager] 拒绝禁用内置管理员账号:" << name;
        return;
    }
    auto user = m_users.value(name);
    user.enabled = enabled;
    m_users.insert(name, user);
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

// ---------------------------------------------------------------------
// 默认值
// ---------------------------------------------------------------------
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

    // P2-44：admin 缺失时重建（而不是「users 整体为空」才建），保证管理入口可恢复
    const QString adminName = QLatin1String(kAdminUserName);
    if (!m_users.contains(adminName)) {
        SecurityUser admin;
        admin.username = adminName;
        admin.passwordSalt = generateSalt();
        admin.passwordHash = strongHash(admin.passwordSalt, QStringLiteral("admin123"));
        admin.role = QStringLiteral("admin");
        admin.enabled = true;
        // S2：种子管理员使用公开默认口令，标记强制改密（登录后必须先修改）
        admin.mustChangePassword = true;
        m_users.insert(adminName, admin);
    } else {
        // P2-43：已存在的 admin 若被降级/禁用，恢复为 admin 角色且保持启用
        SecurityUser admin = m_users.value(adminName);
        bool changed = false;
        admin.username = adminName;
        if (admin.role != QStringLiteral("admin")) { admin.role = QStringLiteral("admin"); changed = true; }
        if (!admin.enabled) { admin.enabled = true; changed = true; }
        if (changed)
            m_users.insert(adminName, admin);
    }
}

// ---------------------------------------------------------------------
// 哈希工具
// ---------------------------------------------------------------------
QString SecurityManager::hashPassword(const QString &password) const
{
    return hashToken(QStringLiteral("pwd:") + password);
}

QString SecurityManager::hashToken(const QString &token) const
{
    return QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString SecurityManager::generateSalt() const
{
    // 随机盐：128 位 UUID（32 个十六进制字符）+ 毫秒时间戳兜底
    return QUuid::createUuid().toString(QUuid::Id128)
           + QString::number(QDateTime::currentMSecsSinceEpoch(), 16);
}

// 旧版加盐哈希（仅用于旧数据兼容校验，不再生成新凭据）
QString SecurityManager::legacySaltedHash(const QString &salt, const QString &secret) const
{
    return hashToken(QStringLiteral("v1:%1:%2").arg(salt, secret));
}

// 新版强哈希：PBKDF2-HMAC-SHA256（"v2:" + hex）
QString SecurityManager::strongHash(const QString &salt, const QString &secret) const
{
    const QByteArray dk = pbkdf2Sha256(secret.toUtf8(), salt.toUtf8(),
                                       kPbkdf2Iterations, kDerivedKeyLength);
    return QStringLiteral("v2:") + QString::fromLatin1(dk.toHex());
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
    if (storedHash.startsWith(QStringLiteral("v2:"))) {
        // 新格式：PBKDF2-HMAC-SHA256（要求同时存在盐）
        if (salt.isEmpty())
            return false;
        return constantTimeEquals(strongHash(salt, secret), storedHash);
    }
    if (!salt.isEmpty()) {
        // 兼容旧版加盐哈希 SHA256("v1:" + salt + ":" + secret)
        return constantTimeEquals(legacySaltedHash(salt, secret), storedHash);
    }
    // 兼容旧版无盐哈希（"pwd:" 前缀裸 SHA256）
    return isLegacyHash(storedHash)
           && constantTimeEquals(hashToken(QStringLiteral("pwd:") + secret), storedHash);
}

// 常量时间比较：逐字节异或累加，比较耗时与内容无关
bool SecurityManager::constantTimeEquals(const QString &a, const QString &b)
{
    const QByteArray ba = a.toUtf8();
    const QByteArray bb = b.toUtf8();
    unsigned int diff = static_cast<unsigned int>(ba.size() ^ bb.size());
    const int n = qMax(ba.size(), bb.size());
    for (int i = 0; i < n; ++i) {
        const unsigned char x = i < ba.size() ? static_cast<unsigned char>(ba.at(i)) : 0u;
        const unsigned char y = i < bb.size() ? static_cast<unsigned char>(bb.at(i)) : 0u;
        diff |= static_cast<unsigned int>(x ^ y);
    }
    return diff == 0;
}

// 审计字段转义
QString SecurityManager::sanitizeAuditField(const QString &text, bool escapeSeparators)
{
    QString out;
    out.reserve(text.size());
    for (const QChar &c : text) {
        const ushort u = c.unicode();
        if (u == '\r' || u == '\n' || u == '\t' || u < 0x20 || u == 0x7F) {
            // 控制字符 → 下划线：防止用换行伪造任意审计行
            out += QLatin1Char('_');
            continue;
        }
        if (escapeSeparators && (u == ' ' || u == '=')) {
            // 标识符字段额外屏蔽分隔符，防止 "key=value" 形式的字段注入
            out += QLatin1Char('_');
            continue;
        }
        out += c;
    }
    return out;
}
