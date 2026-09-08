#ifndef CREDENTIALCODEC_H
#define CREDENTIALCODEC_H

// credentialcodec.h
// 凭据混淆编解码（U1）：避免 MQTT broker 密码等凭据以明文落盘 QSettings。
//
// 说明：这是"防直接可见"的混淆（XOR + Base64，固定内置密钥），不是强加密——
// 没有外部密钥体系时，纯客户端无法做到真正的机密性。威胁模型：
// 防止同机其他用户或随手翻配置文件的人直接读出明文；反编译可还原，已在注释中明示。
// 格式："enc:v1:<base64(xor(utf8(plain)))>"；不带前缀的存量明文在 decode 时原样透传，
// 保存时统一升级为混淆格式。

#include <QByteArray>
#include <QString>

namespace CredentialCodec {

inline const char *prefix() { return "enc:v1:"; }

inline QByteArray xorTransform(const QByteArray &data)
{
    static const QByteArray key = QByteArrayLiteral("FieldLink-Credential-Key-v1");
    QByteArray out = data;
    for (int i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(out.at(i) ^ key.at(i % key.size()));
    return out;
}

inline QString encode(const QString &plain)
{
    if (plain.isEmpty())
        return QString();
    return QString::fromLatin1(prefix())
           + QString::fromLatin1(xorTransform(plain.toUtf8()).toBase64());
}

inline QString decode(const QString &stored)
{
    if (stored.isEmpty())
        return QString();
    if (!stored.startsWith(QString::fromLatin1(prefix())))
        return stored;   // 兼容历史明文
    const QByteArray raw = QByteArray::fromBase64(
        stored.mid(static_cast<int>(qstrlen(prefix()))).toLatin1());
    return QString::fromUtf8(xorTransform(raw));
}

} // namespace CredentialCodec

#endif // CREDENTIALCODEC_H