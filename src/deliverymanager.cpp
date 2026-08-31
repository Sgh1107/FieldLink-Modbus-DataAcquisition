#include "deliverymanager.h"
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QStorageInfo>

DeliveryManager::DeliveryManager(QObject *parent)
    : QObject(parent)
{
    m_checklist << "安装包生成"
                << "Qt依赖打包"
                << "配置迁移验证"
                << "日志打包路径确认"
                << "崩溃日志路径确认"
                << "用户手册"
                << "维护手册"
                << "版本发布说明"
                << "运行环境检查"
                << "回滚方案";
}

QStringList DeliveryManager::checklist() const
{
    return m_checklist;
}

bool DeliveryManager::exportChecklist(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out << "# 产品交付清单\n\n";
    out << "版本: " << versionText() << "\n";
    out << "生成时间: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n\n";
    for (const QString &item : m_checklist)
        out << "- [ ] " << item << "\n";
    return true;
}

QString DeliveryManager::versionText() const
{
    const QString appName = QCoreApplication::applicationName().isEmpty() ? QStringLiteral("modbusmaster") : QCoreApplication::applicationName();
    const QString appVersion = QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("0.1.0") : QCoreApplication::applicationVersion();
    return appName + " " + appVersion;
}

QVector<DeliveryEnvironmentItem> DeliveryManager::checkEnvironment() const
{
    QVector<DeliveryEnvironmentItem> items;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QFileInfo exeInfo(QCoreApplication::applicationFilePath());
    const QStorageInfo storage(appDir);
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    items.append({QStringLiteral("应用可执行文件"), exeInfo.exists(), exeInfo.absoluteFilePath()});
    items.append({QStringLiteral("应用目录可写"), QFileInfo(appDir).isWritable(), appDir});
    items.append({QStringLiteral("历史数据库"), QFileInfo(appDir + "/history.db").exists(), appDir + "/history.db"});
    items.append({QStringLiteral("点位配置"), QFileInfo(appDir + "/points.json").exists(), appDir + "/points.json"});
    items.append({QStringLiteral("模板配置"), QFileInfo(appDir + "/templates.json").exists(), appDir + "/templates.json"});
    items.append({QStringLiteral("日志目录"), QDir(appDir + "/logs").exists(), appDir + "/logs"});
    items.append({QStringLiteral("插件目录"), QDir(appDir + "/plugins").exists(), appDir + "/plugins"});
    items.append({QStringLiteral("磁盘可用空间"), storage.isValid() && storage.bytesAvailable() > 500 * 1024 * 1024, QStringLiteral("可用 %1 MB").arg(storage.bytesAvailable() / 1024 / 1024)});
    items.append({QStringLiteral("windeployqt 环境"), !QStandardPaths::findExecutable("windeployqt").isEmpty(), QStandardPaths::findExecutable("windeployqt")});
    items.append({QStringLiteral("PowerShell 环境"), !QStandardPaths::findExecutable("powershell").isEmpty() || !QStandardPaths::findExecutable("pwsh").isEmpty(), env.value("ComSpec")});
    return items;
}

bool DeliveryManager::exportEnvironmentReport(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true);
    out << "# 运行环境检查报告\n\n";
    out << "版本: " << versionText() << "\n";
    out << "生成时间: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n\n";
    for (const auto &item : checkEnvironment())
        out << "- [" << (item.passed ? "x" : " ") << "] " << item.name << ": " << item.detail << "\n";
    return true;
}

bool DeliveryManager::packageLogs(const QString &targetDir) const
{
    QDir target(targetDir);
    if (!target.exists() && !target.mkpath("."))
        return false;
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString packageDir = target.absoluteFilePath("logs_" + stamp);
    if (!QDir().mkpath(packageDir))
        return false;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList dirs{appDir + "/logs", appDir + "/crashes"};
    bool copied = false;
    for (const QString &dirPath : dirs) {
        QDir source(dirPath);
        if (!source.exists())
            continue;
        const QString subDir = packageDir + "/" + QFileInfo(dirPath).fileName();
        QDir().mkpath(subDir);
        const QFileInfoList files = source.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &info : files)
            copied = QFile::copy(info.absoluteFilePath(), subDir + "/" + info.fileName()) || copied;
    }

    QFile manifest(packageDir + "/manifest.txt");
    if (manifest.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&manifest);
        out << "version=" << versionText() << "\n";
        out << "time=" << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
        out << "source=" << appDir << "\n";
    }
    return copied || QFileInfo(packageDir + "/manifest.txt").exists();
}

bool DeliveryManager::generateReleaseNotes(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true);
    out << "# 版本发布说明\n\n";
    out << "版本: " << versionText() << "\n";
    out << "发布日期: " << QDate::currentDate().toString("yyyy-MM-dd") << "\n\n";
    out << "## 新增能力\n\n- 工业级 Modbus TCP/RTU 数据采集。\n- 点位、模板、报警、趋势、仪表盘和远程服务闭环。\n- 自动化验证测试与交付检查工具。\n\n";
    out << "## 升级说明\n\n- 升级前备份 `points.json`、`templates.json`、`history.db` 和 `profiles` 目录。\n- 使用交付工具执行运行环境检查和日志打包。\n\n";
    out << "## 回滚方案\n\n- 停止应用。\n- 恢复上一版本程序目录和配置备份。\n- 启动后执行验证测试。\n";
    return true;
}

bool DeliveryManager::generateUserManual(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true);
    out << "# 用户手册\n\n";
    out << "## 快速开始\n\n1. 配置串口或 TCP 地址。\n2. 点击连接。\n3. 配置读取地址、寄存器类型和数量。\n4. 使用点位管理、报警配置、实时曲线和仪表盘完成监控。\n\n";
    out << "## 远程服务\n\n在 `Advanced -> 远程服务` 输入 API Token 后启动服务，远程接口需携带 Token。\n\n";
    out << "## 点位管理\n\n在 `Advanced -> 点位管理` 中维护点位，支持生成轮询、报警、曲线和仪表盘绑定。\n";
    return true;
}

bool DeliveryManager::generateMaintenanceManual(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true);
    out << "# 维护手册\n\n";
    out << "## 日常巡检\n\n- 检查日志目录 `logs`。\n- 检查崩溃目录 `crashes`。\n- 检查历史数据库容量。\n- 执行 `Advanced -> 验证测试`。\n\n";
    out << "## 故障处理\n\n- 通信失败：检查端口、从站地址、超时和重试。\n- 数据异常：检查点位数据类型、系数、偏移和报警阈值。\n- 远程访问失败：检查 Token、端口和防火墙。\n\n";
    out << "## 日志打包\n\n使用 `Advanced -> 产品交付 -> 打包日志` 生成日志包并提交给维护人员。\n";
    return true;
}

bool DeliveryManager::generateWindowsPackageScript(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setGenerateByteOrderMark(true);
    out << "param(\n";
    out << "  [string]$BuildDir = \"..\\build\",\n";
    out << "  [string]$ExeName = \"modbusmaster.exe\",\n";
    out << "  [string]$OutputDir = \"..\\release\"\n";
    out << ")\n";
    out << "$ErrorActionPreference = \"Stop\"\n";
    out << "$exe = Join-Path $BuildDir $ExeName\n";
    out << "if (!(Test-Path $exe)) { throw \"Executable not found: $exe\" }\n";
    out << "New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null\n";
    out << "$stage = Join-Path $OutputDir \"package\"\n";
    out << "if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }\n";
    out << "New-Item -ItemType Directory -Force -Path $stage | Out-Null\n";
    out << "Copy-Item $exe $stage\n";
    out << "$deploy = Get-Command windeployqt -ErrorAction SilentlyContinue\n";
    out << "if (!$deploy) { throw \"windeployqt not found in PATH\" }\n";
    out << "& $deploy.Source (Join-Path $stage $ExeName)\n";
    out << "Copy-Item \"..\\用户手册.md\" $stage -ErrorAction SilentlyContinue\n";
    out << "Copy-Item \"..\\维护手册.md\" $stage -ErrorAction SilentlyContinue\n";
    out << "Copy-Item \"..\\版本发布说明.md\" $stage -ErrorAction SilentlyContinue\n";
    out << "$zip = Join-Path $OutputDir (\"modbusmaster_\" + (Get-Date -Format \"yyyyMMdd_HHmmss\") + \".zip\")\n";
    out << "Compress-Archive -Path (Join-Path $stage \"*\") -DestinationPath $zip -Force\n";
    out << "Write-Host \"Package generated: $zip\"\n";
    return true;
}

bool DeliveryManager::generateDeliveryAssets(const QString &targetDir) const
{
    QDir dir(targetDir);
    if (!dir.exists() && !dir.mkpath("."))
        return false;
    QDir deploy(dir.absoluteFilePath("deploy"));
    if (!deploy.exists() && !deploy.mkpath("."))
        return false;
    bool ok = true;
    ok = generateWindowsPackageScript(deploy.absoluteFilePath("package_windows.ps1")) && ok;
    ok = generateReleaseNotes(dir.absoluteFilePath("版本发布说明.md")) && ok;
    ok = generateUserManual(dir.absoluteFilePath("用户手册.md")) && ok;
    ok = generateMaintenanceManual(dir.absoluteFilePath("维护手册.md")) && ok;
    ok = exportChecklist(dir.absoluteFilePath("产品交付清单.md")) && ok;
    ok = exportEnvironmentReport(dir.absoluteFilePath("运行环境检查报告.md")) && ok;
    return ok;
}
