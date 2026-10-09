#ifndef SCANNERDIALOG_H
#define SCANNERDIALOG_H

// Modbus 总线扫描器：在 Unit ID 区间内逐个探测在线从站
//
// 实现基础：对每个 Unit 发送一条原始读请求（QModbusClient::sendRawRequest），
// 收到正常响应即判定该从站在线。扫描为异步状态机，界面不阻塞、可随时停止。

#include <QDialog>
#include <QElapsedTimer>
#include <functional>

QT_BEGIN_NAMESPACE
class QModbusClient;
class QModbusReply;
class QComboBox;
class QSpinBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

class PollManager;

class ScannerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ScannerDialog(std::function<QModbusClient *()> clientProvider,
                           PollManager *pollManager = nullptr,
                           QWidget *parent = nullptr);
    ~ScannerDialog() override;

private slots:
    void startScan();
    void stopScan();
    void onReplyFinished();
    void onGuardTimeout();
    void createPollTaskForSelection();
    void copyResults();

private:
    void sendNextProbe();
    void finishScan();
    void appendFound(int unit, int elapsedMs, const QString &note);
    void updateProgress();

    std::function<QModbusClient *()> m_clientProvider;
    PollManager *m_pollManager = nullptr;

    bool m_running = false;
    int m_total = 0;
    int m_currentUnit = 0;
    int m_currentAttempt = 0;
    int m_scanned = 0;
    int m_found = 0;
    QModbusReply *m_inFlight = nullptr;
    QElapsedTimer m_elapsed;

    QSpinBox *m_unitStartSpin = nullptr;
    QSpinBox *m_unitEndSpin = nullptr;
    QSpinBox *m_attemptsSpin = nullptr;
    QComboBox *m_funcCombo = nullptr;
    QSpinBox *m_addrSpin = nullptr;
    QSpinBox *m_countSpin = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_taskBtn = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_table = nullptr;
    QTimer *m_guardTimer = nullptr;
};

#endif // SCANNERDIALOG_H
