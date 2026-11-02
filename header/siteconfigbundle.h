#ifndef SITECONFIGBUNDLE_H
#define SITECONFIGBUNDLE_H

// siteconfigbundle.h
// 整站配置一键导出 / 导入。
//
// 一个 bundle 是单个 JSON 文件，包含：
//   - 站点配置文件（points.json / templates.json 等）的 base64 内容 + SHA-256；
//   - 应用 QSettings 的全部键值（设备、串口、MQTT、安全、界面等）。
//
// 安全约束（导入时必须全部满足，否则整体拒绝、不落任何文件）：
//   - format / version 校验；
//   - 每个文件路径必须是相对路径，且不含 ".."、盘符、绝对路径、控制字符；
//   - 每个文件的 SHA-256 必须匹配（防止手工改包/传输损坏）；
//   - 先全部解码校验，再写盘；覆盖前把原文件备份到 config_backups/<时间戳>/。

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class QSettings;

class SiteConfigBundle
{
public:
    static QString formatName();
    static int formatVersion();

    // 生成 bundle。relativeFiles 是相对于 baseDir 的路径（如 "points.json"）。
    // 不存在的文件会被跳过并追加到 skipped（不算失败）。
    static QByteArray buildBundle(const QString &baseDir, const QStringList &relativeFiles,
                                  const QSettings &settings, const QString &appVersion,
                                  QStringList *skipped, QString *error);

    static bool writeBundleFile(const QString &filePath, const QByteArray &bundle,
                                QString *error);
    static bool readBundleFile(const QString &filePath, QByteArray *bundle, QString *error);

    // 只做结构/路径/哈希校验，不写盘。root 非空时回传解析后的根对象。
    static bool validateBundle(const QByteArray &bundle, QString *error,
                               QJsonObject *root = nullptr);

    // 校验通过后落盘并应用 settings。
    //   written  : 实际写出的文件绝对路径
    //   warnings : 非致命问题（例如 bundle 内没有 settings）
    //   error   : 致命错误（此时不会写出任何文件，也不会改动 settings）
    static bool applyBundle(const QByteArray &bundle, const QString &targetDir,
                            QSettings &settings, QStringList *written,
                            QStringList *warnings, QString *error);

    // 路径安全校验：空、绝对路径、盘符、包含 ".."、控制字符一律拒绝。
    static bool safeRelativePath(const QString &path);

    // 覆盖前备份目录：<targetDir>/config_backups/<yyyyMMdd_HHmmss_zzz>
    static QString backupDirFor(const QString &targetDir, const QDateTime &now);

    static QString sha256Hex(const QByteArray &data);
};

#endif // SITECONFIGBUNDLE_H
