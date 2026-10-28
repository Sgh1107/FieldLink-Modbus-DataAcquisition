#include "rawrequestdialog.h"
#include "modbusdiagnostics.h"

#include <QModbusClient>
#include <QModbusReply>
#include <QModbusRequest>
#include <QModbusResponse>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QTimer>
#include <QDateTime>
#include <QCoreApplication>

namespace {

struct FuncCodeEntry {
    int code;
    const char *label;
};

// 常用功能码速查表（与 Modbus 规范一致）。
// 字符串用 QT_TRANSLATE_NOOP 标记，运行时按当前语言翻译。
const FuncCodeEntry kFuncCodes[] = {
    { 0x01, QT_TRANSLATE_NOOP("RawRequestDialog", "01 Read Coils") },
    { 0x02, QT_TRANSLATE_NOOP("RawRequestDialog", "02 Read Discrete Inputs") },
    { 0x03, QT_TRANSLATE_NOOP("RawRequestDialog", "03 Read Holding Registers") },
    { 0x04, QT_TRANSLATE_NOOP("RawRequestDialog", "04 Read Input Registers") },
    { 0x05, QT_TRANSLATE_NOOP("RawRequestDialog", "05 Write Single Coil") },
    { 0x06, QT_TRANSLATE_NOOP("RawRequestDialog", "06 Write Single Register") },
    { 0x07, QT_TRANSLATE_NOOP("RawRequestDialog", "07 Read Exception Status") },
    { 0x08, QT_TRANSLATE_NOOP("RawRequestDialog", "08 Diagnostics") },
    { 0x0B, QT_TRANSLATE_NOOP("RawRequestDialog", "0B Get Comm Event Counter") },
    { 0x0C, QT_TRANSLATE_NOOP("RawRequestDialog", "0C Get Comm Event Log") },
    { 0x0F, QT_TRANSLATE_NOOP("RawRequestDialog", "0F Write Multiple Coils") },
    { 0x10, QT_TRANSLATE_NOOP("RawRequestDialog", "10 Write Multiple Registers") },
    { 0x11, QT_TRANSLATE_NOOP("RawRequestDialog", "11 Report Server ID") },
    { 0x16, QT_TRANSLATE_NOOP("RawRequestDialog", "16 Mask Write Register") },
    { 0x17, QT_TRANSLATE_NOOP("RawRequestDialog", "17 Read/Write Multiple Registers") },
    { 0x18, QT_TRANSLATE_NOOP("RawRequestDialog", "18 Read FIFO Queue") },
    { 0x2B, QT_TRANSLATE_NOOP("RawRequestDialog", "2B Read Device Identification (MEI 0E)") },
};

QString timestamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
}

} // namespace

RawRequestDialog::RawRequestDialog(std::function<QModbusClient *()> clientProvider, QWidget *parent)
    : QDialog(parent)
    , m_clientProvider(std::move(clientProvider))
{
    setWindowTitle(tr("Raw Function Code / PDU Sender"));
    resize(760, 560);

    auto *layout = new QVBoxLayout(this);

    // ---------- 请求参数 ----------
    auto *reqGroup = new QGroupBox(tr("Request"), this);
    auto *form = new QFormLayout(reqGroup);

    m_serverSpin = new QSpinBox(reqGroup);
    m_serverSpin->setRange(0, 247);
    m_serverSpin->setValue(1);
    m_serverSpin->setToolTip(tr("Slave address (Unit ID); 0 = broadcast, broadcast writes receive no response"));
    form->addRow(tr("Slave address"), m_serverSpin);

    m_funcCombo = new QComboBox(reqGroup);
    m_funcCombo->setEditable(true);
    for (const FuncCodeEntry &e : kFuncCodes)
        m_funcCombo->addItem(QCoreApplication::translate("RawRequestDialog", e.label), e.code);
    m_funcCombo->setCurrentIndex(2);   // 默认 03 读保持寄存器
    m_funcCombo->setToolTip(tr("Pick from the list or type a hexadecimal function code (e.g. 2B)"));
    form->addRow(tr("Function code"), m_funcCombo);

    m_payloadEdit = new QLineEdit(reqGroup);
    m_payloadEdit->setPlaceholderText(tr("PDU data field (hex), e.g. 00 00 00 0A"));
    form->addRow(tr("Data (PDU)"), m_payloadEdit);

    auto *hint = new QLabel(tr("Examples: <b>03</b> Read Holding Registers → <code>start Hi Lo, count Hi Lo</code>, e.g. <code>00 00 00 0A</code>; <b>08</b> Diagnostics → <code>sub-function Hi Lo, data Hi Lo</code>; <b>16</b> Mask Write → <code>address Hi Lo, AND Hi Lo, OR Hi Lo</code>; <b>2B</b> Read Device Identification → <code>0E 01 00</code>."), reqGroup);
    hint->setWordWrap(true);
    form->addRow(QString(), hint);

    auto *opRow = new QHBoxLayout();
    m_sendBtn = new QPushButton(tr("Send"), reqGroup);
    m_loopCheck = new QCheckBox(tr("Send in loop"), reqGroup);
    m_intervalSpin = new QSpinBox(reqGroup);
    m_intervalSpin->setRange(50, 600000);
    m_intervalSpin->setValue(1000);
    m_intervalSpin->setSuffix(QStringLiteral(" ms"));
    opRow->addWidget(m_sendBtn);
    opRow->addSpacing(12);
    opRow->addWidget(m_loopCheck);
    opRow->addWidget(m_intervalSpin);
    opRow->addStretch();
    form->addRow(QString(), opRow);

    layout->addWidget(reqGroup);

    // ---------- 响应 / 日志 ----------
    auto *respGroup = new QGroupBox(tr("Response & Log"), this);
    auto *respLayout = new QVBoxLayout(respGroup);
    m_log = new QPlainTextEdit(respGroup);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    respLayout->addWidget(m_log);
    layout->addWidget(respGroup, 1);

    // ---------- 状态与关闭 ----------
    auto *bottom = new QHBoxLayout();
    m_status = new QLabel(tr("Sent: 0  OK: 0  Failed: 0"), this);
    bottom->addWidget(m_status);
    bottom->addStretch();
    auto *clearBtn = new QPushButton(tr("Clear Log"), this);
    auto *closeBtn = new QPushButton(tr("Close"), this);
    bottom->addWidget(clearBtn);
    bottom->addWidget(closeBtn);
    layout->addLayout(bottom);

    m_loopTimer = new QTimer(this);
    m_guardTimer = new QTimer(this);
    m_guardTimer->setSingleShot(true);

    connect(m_sendBtn, &QPushButton::clicked, this, &RawRequestDialog::sendRequest);
    connect(m_loopTimer, &QTimer::timeout, this, [this]() { if (!m_busy) sendRequest(); });
    connect(m_guardTimer, &QTimer::timeout, this, &RawRequestDialog::onGuardTimeout);
    connect(m_loopCheck, &QCheckBox::toggled, this, &RawRequestDialog::onLoopToggled);
    connect(clearBtn, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    appendLog(tr("Tip: see the underlined examples above; connect from the main window before sending."), false);
}

RawRequestDialog::~RawRequestDialog()
{
    // 在途请求由 QModbusClient 管理，客户端销毁时会被回收，这里只断开定时器
    if (m_loopTimer) m_loopTimer->stop();
    if (m_guardTimer) m_guardTimer->stop();
}

quint8 RawRequestDialog::currentFunctionCode() const
{
    if (m_funcCombo->currentIndex() >= 0) {
        const QVariant v = m_funcCombo->currentData();
        if (v.isValid())
            return static_cast<quint8>(v.toInt());
    }
    // 手动输入：取首个 token，按十六进制解析
    QString t = m_funcCombo->currentText().trimmed().section(QLatin1Char(' '), 0, 0);
    if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        t = t.mid(2);
    bool ok = false;
    const int code = t.toInt(&ok, 16);
    return ok ? static_cast<quint8>(code & 0xFF) : 0;
}

void RawRequestDialog::appendLog(const QString &line, bool isError)
{
    const QString text = isError
        ? QStringLiteral("<span style=\"color:#e24b4a;\">%1</span>").arg(line.toHtmlEscaped())
        : line.toHtmlEscaped();
    m_log->appendHtml(text);
}

void RawRequestDialog::updateStats()
{
    m_status->setText(tr("Sent: %1  OK: %2  Failed: %3")
                          .arg(m_sent).arg(m_ok).arg(m_fail));
}

void RawRequestDialog::sendRequest()
{
    if (m_busy)
        return;

    QModbusClient *client = m_clientProvider ? m_clientProvider() : nullptr;
    if (!client || client->state() != QModbusDevice::ConnectedState) {
        appendLog(tr("[%1] No device connected; connect from the main window first.").arg(timestamp()), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    QByteArray payload;
    QString err;
    if (!ModbusDiagnostics::parseHexBytes(m_payloadEdit->text(), &payload, &err)) {
        appendLog(tr("[%1] PDU parse failed: %2").arg(timestamp(), err), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    const quint8 code = currentFunctionCode();
    if (!ModbusDiagnostics::isValidFunctionCode(code)) {
        appendLog(tr("[%1] Invalid function code; enter a hex value in 01~7F.").arg(timestamp()), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    const int server = m_serverSpin->value();
    QModbusRequest request(static_cast<QModbusPdu::FunctionCode>(code), payload);
    m_inFlight = client->sendRawRequest(request, server);
    if (!m_inFlight) {
        appendLog(tr("[%1] Send failed: %2").arg(timestamp(), client->errorString()), true);
        ++m_fail;
        updateStats();
        return;
    }

    m_busy = true;
    m_elapsed.restart();
    ++m_sent;
    updateStats();

    connect(m_inFlight, &QModbusReply::finished, this, &RawRequestDialog::onReplyFinished);

    // 超时兜底：防止广播(unit=0)或异常情况下 reply 永不 finished
    const int guard = client->timeout() * (client->numberOfRetries() + 1) + 1500;
    m_guardTimer->start(qMax(1500, guard));
}

void RawRequestDialog::onReplyFinished()
{
    m_guardTimer->stop();
    if (!m_inFlight)
        return;   // 已被兜底定时器处理

    QModbusReply *reply = m_inFlight;
    m_inFlight = nullptr;
    m_busy = false;

    const int ms = static_cast<int>(m_elapsed.elapsed());
    const QString fc = ModbusDiagnostics::hexByte(currentFunctionCode());

    if (reply->error() != QModbusDevice::NoError) {
        ++m_fail;
        appendLog(tr("[%1] Slave %2  FC %3  -> failed: %4  (%5 ms)")
                      .arg(timestamp())
                      .arg(m_serverSpin->value())
                      .arg(fc, reply->errorString())
                      .arg(ms), true);
    } else {
        ++m_ok;
        const QModbusResponse resp = reply->rawResult();
        const QString hex = ModbusDiagnostics::hexBytes(resp.data());
        QString extra;
        if (resp.isException()) {
            const int ex = static_cast<int>(resp.exceptionCode());
            extra = tr("   <- exception code %1 (%2)").arg(ModbusDiagnostics::hexByte(static_cast<quint8>(ex)),
                                                 ModbusDiagnostics::exceptionName(ex));
        }
        appendLog(tr("[%1] Slave %2  FC %3  -> OK  response FC %4  data (%5 bytes) = %6%7  (%8 ms)")
                      .arg(timestamp())
                      .arg(m_serverSpin->value())
                      .arg(fc, ModbusDiagnostics::hexByte(static_cast<quint8>(resp.functionCode())))
                      .arg(resp.data().size())
                      .arg(hex.isEmpty() ? tr("(empty)") : hex)
                      .arg(extra)
                      .arg(ms), false);
    }

    reply->deleteLater();
    updateStats();
}

void RawRequestDialog::onGuardTimeout()
{
    if (!m_inFlight)
        return;
    QModbusReply *reply = m_inFlight;
    m_inFlight = nullptr;
    m_busy = false;
    ++m_fail;
    appendLog(tr("[%1] Slave %2  FC %3  -> timeout, no response (broadcast or silently dropped)")
                  .arg(timestamp())
                  .arg(m_serverSpin->value())
                  .arg(ModbusDiagnostics::hexByte(currentFunctionCode())), true);
    reply->deleteLater();
    updateStats();
}

void RawRequestDialog::onLoopToggled(bool on)
{
    if (on) {
        m_loopTimer->start(qMax(50, m_intervalSpin->value()));
        if (!m_busy)
            sendRequest();
    } else {
        m_loopTimer->stop();
    }
}
