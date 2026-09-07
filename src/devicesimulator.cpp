// devicesimulator.cpp
// 「模拟设备」测试面板实现：用 QProcess 启动/停止 deploy/modbus_tcp_simulator.py。

#include "devicesimulator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
const QString kDefaultPython = QStringLiteral("python");
const int kDefaultPort = 1502;
const int kDefaultUnit = 1;
}

DeviceSimulatorPanel::DeviceSimulatorPanel(QWidget *parent)
    : QDialog(parent)
    , m_pythonEdit(new QLineEdit(this))
    , m_scriptEdit(new QLineEdit(this))
    , m_portSpin(new QSpinBox(this))
    , m_unitSpin(new QSpinBox(this))
    , m_startButton(new QPushButton(QStringLiteral("启动模拟从站"), this))
    , m_stopButton(new QPushButton(QStringLiteral("停止"), this))
    , m_browseButton(new QPushButton(QStringLiteral("浏览..."), this))
    , m_statusLabel(new QLabel(this))
    , m_logView(new QTextBrowser(this))
    , m_userStopped(false)
{
    setWindowTitle(QStringLiteral("模拟设备（Modbus 从站）"));
    resize(560, 460);

    auto *rootLayout = new QVBoxLayout(this);

    // ---- 配置区 ----
    auto *form = new QFormLayout;
    m_pythonEdit->setPlaceholderText(QStringLiteral("python 或绝对路径"));
    m_scriptEdit->setPlaceholderText(QStringLiteral("modbus_tcp_simulator.py 路径"));
    m_portSpin->setRange(1, 65535);
    m_unitSpin->setRange(0, 247);
    form->addRow(QStringLiteral("Python"), m_pythonEdit);
    auto *scriptRow = new QHBoxLayout;
    scriptRow->addWidget(m_scriptEdit, 1);
    scriptRow->addWidget(m_browseButton);
    form->addRow(QStringLiteral("模拟器脚本"), scriptRow);
    form->addRow(QStringLiteral("端口"), m_portSpin);
    form->addRow(QStringLiteral("从站地址"), m_unitSpin);
    rootLayout->addLayout(form);

    // ---- 控制按钮 ----
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_startButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addWidget(m_statusLabel, 1);
    rootLayout->addLayout(buttonRow);

    // ---- 日志 ----
    m_logView->setReadOnly(true);
    rootLayout->addWidget(m_logView, 1);

    m_statusLabel->setText(QStringLiteral("未运行"));
    loadConfig();
    updateButtons();

    connect(m_startButton, &QPushButton::clicked, this, &DeviceSimulatorPanel::onStart);
    connect(m_stopButton, &QPushButton::clicked, this, &DeviceSimulatorPanel::onStop);
    connect(m_browseButton, &QPushButton::clicked, this, &DeviceSimulatorPanel::onBrowseScript);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &DeviceSimulatorPanel::onProcessOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, &DeviceSimulatorPanel::onProcessOutput);
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceSimulatorPanel::onProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, &DeviceSimulatorPanel::onProcessError);

    appendLog(QStringLiteral("提示：启动后主界面连接类型选 TCP，地址填 127.0.0.1:%1，从站地址填 %2。")
                  .arg(m_portSpin->value()).arg(m_unitSpin->value()));
}

DeviceSimulatorPanel::~DeviceSimulatorPanel()
{
    if (m_process.state() != QProcess::NotRunning)
        m_process.kill();   // 防止残留后台 python
}

void DeviceSimulatorPanel::loadConfig()
{
    QSettings settings;
    m_pythonEdit->setText(settings.value(QStringLiteral("sim/python"), kDefaultPython).toString());
    m_portSpin->setValue(settings.value(QStringLiteral("sim/port"), kDefaultPort).toInt());
    m_unitSpin->setValue(settings.value(QStringLiteral("sim/unit"), kDefaultUnit).toInt());

    QString script = settings.value(QStringLiteral("sim/script")).toString();
    if (script.isEmpty())
        script = resolveScriptCandidates();
    m_scriptEdit->setText(script);
}

void DeviceSimulatorPanel::saveConfig()
{
    QSettings settings;
    settings.setValue(QStringLiteral("sim/python"), m_pythonEdit->text().trimmed().isEmpty()
                                                    ? kDefaultPython
                                                    : m_pythonEdit->text().trimmed());
    settings.setValue(QStringLiteral("sim/script"), m_scriptEdit->text().trimmed());
    settings.setValue(QStringLiteral("sim/port"), m_portSpin->value());
    settings.setValue(QStringLiteral("sim/unit"), m_unitSpin->value());
}

QString DeviceSimulatorPanel::resolveScriptCandidates()
{
    // 依次探测可能的脚本位置（开发目录布局：<root>/deploy/modbus_tcp_simulator.py）
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/deploy/modbus_tcp_simulator.py"),
        appDir + QStringLiteral("/../../../deploy/modbus_tcp_simulator.py"),
        QDir::currentPath() + QStringLiteral("/deploy/modbus_tcp_simulator.py"),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QDir::cleanPath(candidate);
    }
    return QString();
}

void DeviceSimulatorPanel::onBrowseScript()
{
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择模拟器脚本"),
        m_scriptEdit->text(), QStringLiteral("Python (*.py)"));
    if (!file.isEmpty())
        m_scriptEdit->setText(file);
}

void DeviceSimulatorPanel::onStart()
{
    if (m_process.state() != QProcess::NotRunning)
        return;
    if (m_scriptEdit->text().trimmed().isEmpty() || !QFileInfo::exists(m_scriptEdit->text().trimmed())) {
        appendLog(QStringLiteral("错误：模拟器脚本路径无效，请选择正确的 modbus_tcp_simulator.py"));
        return;
    }

    saveConfig();
    m_userStopped = false;
    m_logView->clear();

    QStringList args;
    args << m_scriptEdit->text().trimmed()
         << QStringLiteral("--port") << QString::number(m_portSpin->value())
         << QStringLiteral("--unit") << QString::number(m_unitSpin->value());

    // 关掉进程可能的环境依赖差异：继承 PATH，保证能找到 python
    m_process.start(m_pythonEdit->text().trimmed(), args);
    updateButtons();   // 启动失败由 errorOccurred 信号统一处理并回滚按钮状态
}

void DeviceSimulatorPanel::onStop()
{
    m_userStopped = true;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(1500))
            m_process.kill();
    }
    m_statusLabel->setText(QStringLiteral("已停止"));
    updateButtons();
}

void DeviceSimulatorPanel::onProcessOutput()
{
    const QByteArray out = m_process.readAllStandardOutput();
    const QByteArray err = m_process.readAllStandardError();
    if (!out.isEmpty())
        appendLog(QString::fromLocal8Bit(out));
    if (!err.isEmpty())
        appendLog(QStringLiteral("<span style=\"color:#c0392b\">%1</span>").arg(QString::fromLocal8Bit(err).toHtmlEscaped()));
}

void DeviceSimulatorPanel::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    appendLog(QStringLiteral("模拟从站进程已退出（exit code %1）。").arg(exitCode));
    if (!m_userStopped) {
        // 非用户主动停止 → 通常是端口被占用等启动失败
        appendLog(QStringLiteral("提示：若提示端口占用，说明该端口已有服务在运行，请换一个端口。"));
    }
    m_statusLabel->setText(QStringLiteral("未运行"));
    updateButtons();
}

void DeviceSimulatorPanel::onProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        appendLog(QStringLiteral("错误：无法启动模拟器，请检查 Python/脚本路径。"));
        m_statusLabel->setText(QStringLiteral("启动失败"));
        updateButtons();
    }
}

void DeviceSimulatorPanel::updateButtons()
{
    const bool running = m_process.state() != QProcess::NotRunning;
    m_startButton->setEnabled(!running);
    m_stopButton->setEnabled(running);
    m_pythonEdit->setEnabled(!running);
    m_scriptEdit->setEnabled(!running);
    m_browseButton->setEnabled(!running);
    m_portSpin->setEnabled(!running);
    m_unitSpin->setEnabled(!running);
}

void DeviceSimulatorPanel::appendLog(const QString &text)
{
    m_logView->append(text);
}
