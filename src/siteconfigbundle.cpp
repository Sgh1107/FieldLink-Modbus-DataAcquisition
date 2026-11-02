#include "siteconfigbundle.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QVariant>
#include <QVector>

QString SiteConfigBundle::formatName()
{
    return QStringLiteral("fieldlink-site-config");
}

int SiteConfigBundle::formatVersion()
{
    return 1;
}

QString SiteConfigBundle::sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool SiteConfigBundle::safeRelativePath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return false;
    if (trimmed != path)
        return false;   // 首尾空白容易在 Windows 上产生歧义文件名，直接拒绝

    // 绝对路径 / 盘符 / UNC
    if (trimmed.startsWith(QLatin1Char('/')) || trimmed.startsWith(QLatin1Char('\\')))
        return false;
    if (trimmed.size() >= 2 && trimmed.at(1) == QLatin1Char(':'))
        return false;
    if (trimmed.startsWith(QLatin1String("//")) || trimmed.startsWith(QLatin1String("\\\\")))
        return false;
    if (trimmed.contains(QLatin1String("//")) || trimmed.contains(QLatin1String("\\\\")))
        return false;

    // 目录穿越
    const QStringList parts = trimmed.split(QRegularExpression(QStringLiteral("[\\\\/]")),
                                            Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    }
    if (trimmed.contains(QStringLiteral("..")))
        return false;

    for (const QChar &c : trimmed) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            return false;
    }
    return true;
}

QByteArray SiteConfigBundle::buildBundle(const QString &baseDir, const QStringList &relativeFiles,
                                         const QSettings &settings, const QString &appVersion,
                                         QStringList *skipped, QString *error)
{
    QJsonObject root;
    root[QStringLiteral("format")] = formatName();
    root[QStringLiteral("version")] = formatVersion();
    root[QStringLiteral("created")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    root[QStringLiteral("appVersion")] = appVersion;

    QJsonArray files;
    const QDir dir(baseDir);
    for (const QString &relative : relativeFiles) {
        if (!safeRelativePath(relative)) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "unsafe file path: %1").arg(relative);
            return QByteArray();
        }
        const QString absolute = dir.absoluteFilePath(relative);
        QFile file(absolute);
        if (!file.exists()) {
            if (skipped)
                skipped->append(relative);
            continue;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "cannot read %1: %2").arg(relative, file.errorString());
            return QByteArray();
        }
        const QByteArray data = file.readAll();
        file.close();

        QJsonObject entry;
        entry[QStringLiteral("path")] = relative;
        entry[QStringLiteral("size")] = data.size();
        entry[QStringLiteral("sha256")] = sha256Hex(data);
        entry[QStringLiteral("data")] = QString::fromLatin1(data.toBase64());
        files.append(entry);
    }
    root[QStringLiteral("files")] = files;

    QJsonObject settingsObject;
    for (const QString &key : settings.allKeys()) {
        const QVariant value = settings.value(key);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const int typeId = value.typeId();
#else
        const int typeId = value.type();
#endif
        switch (typeId) {
        case QMetaType::Bool:
            settingsObject[key] = value.toBool();
            break;
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            settingsObject[key] = static_cast<double>(value.toLongLong());
            break;
        case QMetaType::Double:
        case QMetaType::Float:
            settingsObject[key] = value.toDouble();
            break;
        default:
            settingsObject[key] = value.toString();
            break;
        }
    }
    root[QStringLiteral("settings")] = settingsObject;

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool SiteConfigBundle::writeBundleFile(const QString &filePath, const QByteArray &bundle,
                                       QString *error)
{
    const QFileInfo info(filePath);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = QCoreApplication::translate("SiteConfigBundle", "cannot create directory: %1").arg(info.absolutePath());
        return false;
    }

    // QSaveFile：写临时文件 + 原子 rename，导出中断不会留下半个 bundle。
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.write(bundle) != bundle.size()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool SiteConfigBundle::readBundleFile(const QString &filePath, QByteArray *bundle, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (bundle)
        *bundle = file.readAll();
    return true;
}

bool SiteConfigBundle::validateBundle(const QByteArray &bundle, QString *error, QJsonObject *root)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bundle, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QCoreApplication::translate("SiteConfigBundle", "invalid JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject object = doc.object();
    if (object.value(QStringLiteral("format")).toString() != formatName()) {
        if (error)
            *error = QCoreApplication::translate("SiteConfigBundle", "not a FieldLink site configuration bundle");
        return false;
    }

    const int version = object.value(QStringLiteral("version")).toInt(-1);
    if (version < 1 || version > formatVersion()) {
        if (error)
            *error = QCoreApplication::translate("SiteConfigBundle", "unsupported bundle version: %1").arg(version);
        return false;
    }

    const QJsonValue filesValue = object.value(QStringLiteral("files"));
    if (!filesValue.isArray()) {
        if (error)
            *error = QCoreApplication::translate("SiteConfigBundle", "missing files array");
        return false;
    }

    QStringList seen;
    for (const QJsonValue &value : filesValue.toArray()) {
        if (!value.isObject()) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "invalid file entry");
            return false;
        }
        const QJsonObject entry = value.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (!safeRelativePath(path)) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "unsafe file path: %1").arg(path);
            return false;
        }
        if (seen.contains(path)) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "duplicate file path: %1").arg(path);
            return false;
        }
        seen.append(path);

        const QString expected = entry.value(QStringLiteral("sha256")).toString().toLower();
        if (expected.size() != 64) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "missing or invalid checksum for %1").arg(path);
            return false;
        }
        const QByteArray data = QByteArray::fromBase64(
            entry.value(QStringLiteral("data")).toString().toLatin1());
        const QString actual = sha256Hex(data);
        if (actual != expected) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "checksum mismatch for %1").arg(path);
            return false;
        }
        const int expectedSize = entry.value(QStringLiteral("size")).toInt(-1);
        if (expectedSize >= 0 && expectedSize != data.size()) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "size mismatch for %1").arg(path);
            return false;
        }
    }

    const QJsonValue settingsValue = object.value(QStringLiteral("settings"));
    if (!settingsValue.isUndefined() && !settingsValue.isObject()) {
        if (error)
            *error = QCoreApplication::translate("SiteConfigBundle", "settings must be an object");
        return false;
    }

    if (root)
        *root = object;
    return true;
}

QString SiteConfigBundle::backupDirFor(const QString &targetDir, const QDateTime &now)
{
    return QDir(targetDir).absoluteFilePath(
        QStringLiteral("config_backups/%1").arg(now.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
}

bool SiteConfigBundle::applyBundle(const QByteArray &bundle, const QString &targetDir,
                                   QSettings &settings, QStringList *written,
                                   QStringList *warnings, QString *error)
{
    QJsonObject root;
    if (!validateBundle(bundle, error, &root))
        return false;

    struct PendingFile {
        QString relative;
        QByteArray data;
    };
    QVector<PendingFile> pending;
    for (const QJsonValue &value : root.value(QStringLiteral("files")).toArray()) {
        const QJsonObject entry = value.toObject();
        PendingFile file;
        file.relative = entry.value(QStringLiteral("path")).toString();
        file.data = QByteArray::fromBase64(
            entry.value(QStringLiteral("data")).toString().toLatin1());
        pending.append(file);
    }

    // 先检查所有目标是否可写（父目录可创建），再开始覆盖，尽量避免"写了一半"。
    const QDir target(targetDir);
    for (const PendingFile &file : pending) {
        const QString absolute = target.absoluteFilePath(file.relative);
        const QFileInfo info(absolute);
        if (!QDir().mkpath(info.absolutePath())) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "cannot create directory: %1").arg(info.absolutePath());
            return false;
        }
    }

    const QString backupDir = backupDirFor(targetDir, QDateTime::currentDateTime());
    QStringList writtenFiles;

    for (const PendingFile &file : pending) {
        const QString absolute = target.absoluteFilePath(file.relative);
        const QFileInfo info(absolute);

        // 覆盖前备份原文件（相对目录结构保持不变）。
        if (info.exists()) {
            const QString backupPath = QDir(backupDir).absoluteFilePath(file.relative);
            if (!QDir().mkpath(QFileInfo(backupPath).absolutePath())
                || !QFile::copy(absolute, backupPath)) {
                if (error)
                    *error = QCoreApplication::translate("SiteConfigBundle", "cannot back up %1").arg(file.relative);
                return false;
            }
        }

        // 写临时文件后原子替换；Windows 上 rename 不能覆盖已存在文件。
        const QString tempPath = absolute + QStringLiteral(".tmp");
        QFile temp(tempPath);
        if (!temp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = temp.errorString();
            return false;
        }
        if (temp.write(file.data) != file.data.size()) {
            if (error)
                *error = temp.errorString();
            temp.close();
            QFile::remove(tempPath);
            return false;
        }
        temp.close();

        if (info.exists() && !QFile::remove(absolute)) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "cannot replace %1").arg(file.relative);
            QFile::remove(tempPath);
            return false;
        }
        if (!QFile::rename(tempPath, absolute)) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "cannot write %1").arg(file.relative);
            QFile::remove(tempPath);
            return false;
        }
        writtenFiles.append(absolute);
    }

    const QJsonValue settingsValue = root.value(QStringLiteral("settings"));
    if (settingsValue.isObject()) {
        const QJsonObject settingsObject = settingsValue.toObject();
        for (auto it = settingsObject.constBegin(); it != settingsObject.constEnd(); ++it)
            settings.setValue(it.key(), it.value().toVariant());
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            if (error)
                *error = QCoreApplication::translate("SiteConfigBundle", "cannot apply settings");
            return false;
        }
    } else if (warnings) {
        warnings->append(QCoreApplication::translate("SiteConfigBundle", "bundle contains no settings"));
    }

    if (written)
        *written = writtenFiles;
    return true;
}
