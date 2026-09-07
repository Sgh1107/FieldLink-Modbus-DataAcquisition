#ifndef DEVICESIMULATOR_H
#define DEVICESIMULATOR_H

// devicesimulator.h
// 「模拟设备」测试面板：在 FieldLink 内一键启动/停止仓库自带的 Modbus 从站模拟器
// （deploy/modbus_tcp_simulator.py，独立 Python 实现，作为交叉验证参照物）。
//
// 用途：日常联调不再需要手动另开终端跑 Python；点一下即可得到一个真实 TCP 从站，
//       主界面按 TCP 连接 127.0.0.1:<port> 即可走完采集/轮询/报警/MQTT 全链路。
// 说明：Python 脚本作为独立子进程运行（QProcess），与 FieldLink 的 Modbus 主机代码
//       相互独立，因此能起到"独立实现互验"的作用（避免同进程自连的自洽假象）。

#include <QDialog>
#include <QProcess>

class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QTextBrowser;

class DeviceSimulatorPanel : public QDialog
{
    Q_OBJECT

public:
    explicit DeviceSimulatorPanel(QWidget *parent = nullptr);
    ~DeviceSimulatorPanel();

private slots:
    void onStart();
    void onStop();
    void onBrowseScript();
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void updateButtons();

private:
    void loadConfig();
    void saveConfig();
    QString resolveScriptCandidates();
    void appendLog(const QString &text);

    QProcess m_process;
    QLineEdit *m_pythonEdit;
    QLineEdit *m_scriptEdit;
    QSpinBox *m_portSpin;
    QSpinBox *m_unitSpin;
    QPushButton *m_startButton;
    QPushButton *m_stopButton;
    QPushButton *m_browseButton;
    QLabel *m_statusLabel;
    QTextBrowser *m_logView;
    bool m_userStopped;
};

#endif // DEVICESIMULATOR_H
