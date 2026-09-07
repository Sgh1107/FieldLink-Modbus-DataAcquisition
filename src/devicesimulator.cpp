// devicesimulator.cpp
// 「模拟设备」测试面板实现：用 QProcess 启动/停止 slave/modbus_tcp_simulator.py。
// 界面文案统一使用 tr()，中文翻译见 translations/fieldlink_zh_CN.ts（DeviceSimulatorPanel 上下文）。

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
    , m_startButton(new QPushButton(this))
    , m_stopButton(new QPushButton(this))
    , m_browseButton(new QPushButton(this))
    , m_statusLabel(new QLabel(this))
    , m_logView(new QTextBrowser(this))
    , m_userStopped(false)
{
    setWindowTitle(tr("Device Simulator (Modbus Slave)"));
    resize(560, 460);

    auto *rootLayout = new QVBoxLayout(this);

    // ---- 配置区 ----
    auto *form = new QFormLayout;
    m_pythonEdit->setPlaceholderText(tr("python or absolute path"));
    m_scriptEdit->setPlaceholderText(tr("Path to modbus_tcp_simulator.py"));
    m_portSpin->setRange(1, 65535);
    m_unitSpin->setRange(0, 247);
    form->addRow(tr("Python"), m_pythonEdit);
    auto *scriptRow = new QHBoxLayout;
    scriptRow->addWidget(m_scriptEdit, 1);
    scriptRow->addWidget(m_browseButton);
    form->addRow(tr("Simulator script"), scriptRow);
    form->addRow(tr("Port"), m_portSpin);
    form->addRow(tr("Slave address"), m_unitSpin);
    rootLayout->addLayout(form);

    // ---- 控制按钮 ----
    m_startButton->setText(tr("Start slave simulator"));
    m_stopButton->setText(tr("Stop"));
    m_browseButton->setText(tr("Browse..."));
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_startButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addWidget(m_statusLabel, 1);
    rootLayout->addLayout(buttonRow);

    // ---- 日志 ----
    m_logView->setReadOnly(true);
    rootLayout->addWidget(m_logView, 1);

    m_statusLabel->setText(tr("Not running"));
    loadConfig();
    updateButtons();

    connect(m_startButton, &QPushButton::clicked, this, &DeviceSimulatorPanel::onStart);
    connect(m_stopButton, &QPushButton::clicked, this, &DeviceSimulatorPanel::onStop);
    connect(m_browseButton, &QPushButton::clicked, this, &DeviceSimulatorPanel::onBrowseScript);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &DeviceSimulatorPanel::onProcessOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, &DeviceSimulatorPanel::onProcessOutput);
    connect(&m_process, &QProcess::started, this, [this]() {
        m_statusLabel->setText(tr("Running (PID %1)").arg(m_process.processId()));
        // 启动信息回显：连接参数 + 模拟内容说明
        appendLog(tr("=== Slave simulator running ==="));
        appendLog(tr("Listen: 127.0.0.1:%1   Slave unit: %2").arg(m_portSpin->value()).arg(m_unitSpin->value()));
        appendLog(tr("Simulated data: reg0 = temperature sine (15.0~35.0°C x10), reg1 = random walk, reg2 = 42, reg3-99 = pattern"));
        appendLog(tr("Main window connect: TCP 127.0.0.1:%1, slave address %2")
                      .arg(m_portSpin->value()).arg(m_unitSpin->value()));
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceSimulatorPanel::onProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, &DeviceSimulatorPanel::onProcessError);

    appendLog(tr("Tip: after starting, connect the main window via TCP to 127.0.0.1:%1 with slave address %2.")
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
    // 依次探测可能的脚本位置（开发目录布局：<root>/slave/modbus_tcp_simulator.py）
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/slave/modbus_tcp_simulator.py"),
        appDir + QStringLiteral("/../../../slave/modbus_tcp_simulator.py"),
        QDir::currentPath() + QStringLiteral("/slave/modbus_tcp_simulator.py"),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QDir::cleanPath(candidate);
    }
    return QString();
}

void DeviceSimulatorPanel::onBrowseScript()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Select simulator script"),
        m_scriptEdit->text(), tr("Python (*.py)"));
    if (!file.isEmpty())
        m_scriptEdit->setText(file);
}

void DeviceSimulatorPanel::onStart()
{
    if (m_process.state() != QProcess::NotRunning)
        return;
    if (m_scriptEdit->text().trimmed().isEmpty() || !QFileInfo::exists(m_scriptEdit->text().trimmed())) {
        appendLog(tr("Error: invalid simulator script path. Please select modbus_tcp_simulator.py."));
        return;
    }

    saveConfig();
    m_userStopped = false;
    m_logView->clear();

    QStringList args;
    args << QStringLiteral("-u")   // 无缓冲输出：运行时日志实时显示在面板日志区
         << m_scriptEdit->text().trimmed()
         << QStringLiteral("--port") << QString::number(m_portSpin->value())
         << QStringLiteral("--unit") << QString::number(m_unitSpin->value());

    // 继承系统 PATH 以解析 python；启动失败由 errorOccurred 信号统一处理并回滚按钮状态
    m_process.start(m_pythonEdit->text().trimmed(), args);
    updateButtons();
}

void DeviceSimulatorPanel::onStop()
{
    m_userStopped = true;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();   // Windows 控制台进程对 terminate 常不响应，超时后 kill 兜底
        if (!m_process.waitForFinished(1500))
            m_process.kill();
    }
    m_statusLabel->setText(tr("Stopped"));
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
    appendLog(tr("Simulator process exited (exit code %1).").arg(exitCode));
    if (!m_userStopped) {
        // 非用户主动停止 → 通常是端口被占用等启动失败
        appendLog(tr("Hint: if the port is occupied, another service is already listening — try a different port."));
    }
    m_statusLabel->setText(tr("Not running"));
    updateButtons();
}

void DeviceSimulatorPanel::onProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        appendLog(tr("Error: cannot start the simulator. Please check the Python/script path."));
        m_statusLabel->setText(tr("Failed to start"));
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
