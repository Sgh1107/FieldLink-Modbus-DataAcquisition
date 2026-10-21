#ifndef CREDENTIALCODEC_H
#define CREDENTIALCODEC_H

// credentialcodec.h
// 凭据混淆编解码：避免 MQTT broker 密码等凭据以明文落盘 QSettings。
//
// P2-46 / P2-47 修复要点：
//   * 旧实现使用「硬编码在源码里」的固定 XOR 密钥（27 字节重复密钥流）——
//     攻击者用任意已知明文（crib）即可还原整条密钥流，落盘口令等同明文。
//     新实现中源码不含任何密钥字面量：主密钥在首次运行时由 CSPRNG 随机生成
//     （32 字节），保存在用户可写目录（QStandardPaths::AppDataLocation），
//     每台机器/每个用户一份。
//   * 正文用主密钥派生的 SHA-256 计数器密钥流做 XOR（不再重复使用短密钥），
//     并附带 16 字节 HMAC-SHA256 标签：密文被篡改/损坏时可被发现。
//   * decode 使用 Base64AbortOnBase64DecodingErrors：Base64 非法时返回空串，
//     并通过 Status 区分「明文透传 / 正常解密 / 密文损坏」。
//
// 格式："enc:v1:2:<base64(tag||ciphertext)>"
// 兼容性说明：不带前缀的存量明文在 decode 时原样透传；
//   旧版 "enc:v1:<base64>" 密文使用了已公开的硬编码密钥，无法再解出，
//   decode 返回空串并把状态置为 Damaged（调用方需提示用户重新输入凭据）。
//   保存时统一升级为新格式。

#include <QByteArray>
#include <QString>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

namespace CredentialCodec {

// 解码结果状态（让调用方能区分「密码错」与「密文损坏」）
enum class Status {
    Ok,         // 正常解密
    Empty,      // 空串
    Plaintext,  // 历史明文（无前缀）原样透传
    Damaged     // 密文损坏 / 版本不支持 / 标签校验失败
};

inline const char *prefix() { return "enc:v1:"; }

// 主密钥长度（字节）与完整性标签长度（字节）
inline int masterKeyLength() { return 32; }
inline int tagLength() { return 16; }

// ---------------------------------------------------------------------
// 主密钥：随机生成 + 持久化（源码中不含任何密钥字面量）
// ---------------------------------------------------------------------
inline QString masterKeyPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    // 未设置 organization/application 时 AppDataLocation 就是用户漫游目录根，
    // 追加一级子目录，避免把密钥文件直接写进用户根目录
    if (QCoreApplication::organizationName().isEmpty())
        base += QLatin1String("/FieldLink");
    return base + QLatin1String("/credentials.key");
}

inline QByteArray randomBytes(int size)
{
    QByteArray out(size, '\0');
    QRandomGenerator *rng = QRandomGenerator::system();
    for (int i = 0; i < size; ++i)
        out[i] = static_cast<char>(rng->bounded(256));
    return out;
}

inline QByteArray masterKey()
{
    static QByteArray key;
    static bool initialized = false;
    if (initialized)
        return key;
    initialized = true;

    const QString path = masterKeyPath();
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray data = file.readAll();
        file.close();
        if (data.size() == masterKeyLength()) {
            key = data;
            return key;
        }
    }

    // 首次运行（或密钥文件损坏）：生成随机主密钥并保存
    key = randomBytes(masterKeyLength());
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(key);
        out.flush();
        out.close();
        // 尽量只允许属主读写（POSIX 有效；Windows 上为尽力而为）
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    } else {
        qWarning() << "[CredentialCodec] 主密钥无法持久化，编码结果仅在本进程内有效:" << path;
    }
    return key;
}

// ---------------------------------------------------------------------
// 密码学工具
// ---------------------------------------------------------------------
inline QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
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

// 由主密钥派生计数器模式的密钥流（不使用重复短密钥）
inline QByteArray deriveKeystream(const QByteArray &key, int length)
{
    QByteArray out;
    out.reserve(length);
    quint32 counter = 0;
    while (out.size() < length) {
        QByteArray block = key;
        block.append(QByteArrayLiteral("fieldlink.credential.keystream"));  // 域分离标签（非密钥）
        block.append(static_cast<char>((counter >> 24) & 0xFF));
        block.append(static_cast<char>((counter >> 16) & 0xFF));
        block.append(static_cast<char>((counter >> 8) & 0xFF));
        block.append(static_cast<char>(counter & 0xFF));
        out.append(QCryptographicHash::hash(block, QCryptographicHash::Sha256));
        ++counter;
    }
    out.truncate(length);
    return out;
}

// 常量时间比较
inline bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    unsigned int diff = static_cast<unsigned int>(a.size() ^ b.size());
    const int n = qMax(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        const unsigned char x = i < a.size() ? static_cast<unsigned char>(a.at(i)) : 0u;
        const unsigned char y = i < b.size() ? static_cast<unsigned char>(b.at(i)) : 0u;
        diff |= static_cast<unsigned int>(x ^ y);
    }
    return diff == 0;
}

// ---------------------------------------------------------------------
// 编解码
// ---------------------------------------------------------------------
inline QString encode(const QString &plain)
{
    if (plain.isEmpty())
        return QString();

    const QByteArray key = masterKey();
    const QByteArray data = plain.toUtf8();
    const QByteArray stream = deriveKeystream(key, data.size());
    QByteArray cipher(data.size(), '\0');
    for (int i = 0; i < data.size(); ++i)
        cipher[i] = static_cast<char>(data.at(i) ^ stream.at(i));

    QByteArray tag = hmacSha256(key, cipher);
    tag.truncate(tagLength());

    return QString::fromLatin1(prefix()) + QStringLiteral("2:")
           + QString::fromLatin1((tag + cipher).toBase64());
}

inline QString decode(const QString &stored, Status *status)
{
    if (status)
        *status = Status::Empty;
    if (stored.isEmpty())
        return QString();

    const QString pfx = QString::fromLatin1(prefix());
    if (!stored.startsWith(pfx)) {
        // 兼容历史明文：原样透传
        if (status)
            *status = Status::Plaintext;
        return stored;
    }

    const QString body = stored.mid(pfx.size());
    if (!body.startsWith(QStringLiteral("2:"))) {
        // 旧格式（硬编码 XOR 密钥）——该密钥已公开，不再保留，无法解出
        if (status)
            *status = Status::Damaged;
        return QString();
    }

    // P2-47：Base64 非法时不再静默返回垃圾字节
    // （Qt 6 中该标志名为 QByteArray::AbortOnBase64DecodingErrors）
    const QByteArray blob = QByteArray::fromBase64(body.mid(2).toLatin1(),
                                                   QByteArray::AbortOnBase64DecodingErrors);
    if (blob.size() < tagLength()) {
        if (status)
            *status = Status::Damaged;
        return QString();
    }

    const QByteArray tag = blob.left(tagLength());
    const QByteArray cipher = blob.mid(tagLength());
    const QByteArray key = masterKey();

    QByteArray expect = hmacSha256(key, cipher);
    expect.truncate(tagLength());
    if (!constantTimeEquals(expect, tag)) {
        // 标签不匹配 → 密文被篡改或主密钥已变更
        if (status)
            *status = Status::Damaged;
        return QString();
    }

    const QByteArray stream = deriveKeystream(key, cipher.size());
    QByteArray plain(cipher.size(), '\0');
    for (int i = 0; i < cipher.size(); ++i)
        plain[i] = static_cast<char>(cipher.at(i) ^ stream.at(i));

    if (status)
        *status = Status::Ok;
    return QString::fromUtf8(plain);
}

inline QString decode(const QString &stored)
{
    return decode(stored, static_cast<Status *>(nullptr));
}

} // namespace CredentialCodec

#endif // CREDENTIALCODEC_H
