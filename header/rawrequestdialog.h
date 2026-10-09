#ifndef RAWREQUESTDIALOG_H
#define RAWREQUESTDIALOG_H

// 原始功能码 / 原始报文发送工具窗
//
// 用途：直接发送任意 Modbus 功能码（PDU），覆盖 Qt SerialBus 未封装的高阶功能：
//   FC07 读异常状态 / FC08 诊断 / FC0B·0C 通信事件 / FC11 报告从站ID /
//   FC16(22) 掩码写寄存器 / FC17(23) 读写多寄存器 / FC18(24) 读FIFO / FC2B·0E 读设备标识
// 实现基础：QModbusClient::sendRawRequest(QModbusRequest, serverAddress)

#include <QDialog>
#include <QByteArray>
#include <QElapsedTimer>
#include <QModbusPdu>
#include <functional>

QT_BEGIN_NAMESPACE
class QModbusClient;
class QModbusReply;
class QComboBox;
class QSpinBox;
class QLineEdit;
class QCheckBox;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTimer;
QT_END_NAMESPACE

class RawRequestDialog : public QDialog
{
    Q_OBJECT

public:
    // clientProvider 用于每次发送时重新获取当前 Modbus 客户端。
    // 主窗口在自动重连时会销毁并重建客户端，直接缓存裸指针会失效，故用回调取。
    explicit RawRequestDialog(std::function<QModbusClient *()> clientProvider,
                              QWidget *parent = nullptr);
    ~RawRequestDialog() override;

private slots:
    void sendRequest();
    void onReplyFinished();
    void onLoopToggled(bool on);
    void onGuardTimeout();

private:
    void appendLog(const QString &line, bool isError = false);
    void updateStats();
    quint8 currentFunctionCode() const;

    std::function<QModbusClient *()> m_clientProvider;
    QModbusReply *m_inFlight = nullptr;
    bool m_busy = false;
    QElapsedTimer m_elapsed;
    int m_sent = 0;
    int m_ok = 0;
    int m_fail = 0;

    QSpinBox *m_serverSpin = nullptr;
    QComboBox *m_funcCombo = nullptr;
    QLineEdit *m_payloadEdit = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QCheckBox *m_loopCheck = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_loopTimer = nullptr;
    QTimer *m_guardTimer = nullptr;
};

#endif // RAWREQUESTDIALOG_H
