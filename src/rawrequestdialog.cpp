#include "rawrequestdialog.h"

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
#include <QRegularExpression>

namespace {

struct FuncCodeEntry {
    int code;
    const char *label;
};

// 常用功能码速查表（与 Modbus 规范一致）
const FuncCodeEntry kFuncCodes[] = {
    { 0x01, "01 读线圈 Read Coils" },
    { 0x02, "02 读离散输入 Read Discrete Inputs" },
    { 0x03, "03 读保持寄存器 Read Holding Registers" },
    { 0x04, "04 读输入寄存器 Read Input Registers" },
    { 0x05, "05 写单个线圈 Write Single Coil" },
    { 0x06, "06 写单个寄存器 Write Single Register" },
    { 0x07, "07 读异常状态 Read Exception Status" },
    { 0x08, "08 诊断 Diagnostics" },
    { 0x0B, "0B 取通信事件计数 Get Comm Event Counter" },
    { 0x0C, "0C 取通信事件日志 Get Comm Event Log" },
    { 0x0F, "0F 写多个线圈 Write Multiple Coils" },
    { 0x10, "10 写多个寄存器 Write Multiple Registers" },
    { 0x11, "11 报告从站ID Report Server ID" },
    { 0x16, "16 掩码写寄存器 Mask Write Register" },
    { 0x17, "17 读写多个寄存器 Read/Write Multiple Registers" },
    { 0x18, "18 读 FIFO 队列 Read FIFO Queue" },
    { 0x2B, "2B 读设备标识 Read Device Identification (MEI 0E)" },
};

QString exceptionName(int code)
{
    switch (code) {
    case 0x01: return QStringLiteral("非法功能");
    case 0x02: return QStringLiteral("非法数据地址");
    case 0x03: return QStringLiteral("非法数据值");
    case 0x04: return QStringLiteral("从站设备故障");
    case 0x05: return QStringLiteral("确认(处理中)");
    case 0x06: return QStringLiteral("从站设备忙");
    case 0x08: return QStringLiteral("存储奇偶校验错");
    case 0x0A: return QStringLiteral("网关路径不可用");
    case 0x0B: return QStringLiteral("网关目标设备响应失败");
    default:   return QStringLiteral("未知异常");
    }
}

// 把 "0x00 0x0A" / "00,0a" / "000A" 之类文本解析为字节数组
bool parseHexBytes(const QString &text, QByteArray *out, QString *error)
{
    QString s = text;
    s.remove(QRegularExpression(QStringLiteral("[\\s,;:_-]")));
    s.remove(QRegularExpression(QStringLiteral("0[xX]")));
    if (s.isEmpty()) {
        out->clear();
        return true;   // 允许空 PDU（部分功能码无数据域）
    }
    if (s.size() % 2 != 0) {
        if (error) *error = QStringLiteral("十六进制字符数必须为偶数（每字节 2 个字符）");
        return false;
    }
    for (int i = 0; i < s.size(); i += 2) {
        bool ok = false;
        const int b = s.mid(i, 2).toInt(&ok, 16);
        if (!ok) {
            if (error) *error = QStringLiteral("非法十六进制片段：%1").arg(s.mid(i, 2));
            return false;
        }
        out->append(static_cast<char>(b & 0xFF));
    }
    return true;
}

QString hexByte(quint8 v)
{
    return QString::number(v, 16).rightJustified(2, QChar('0')).toUpper();
}

QString timestamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
}

} // namespace

RawRequestDialog::RawRequestDialog(std::function<QModbusClient *()> clientProvider, QWidget *parent)
    : QDialog(parent)
    , m_clientProvider(std::move(clientProvider))
{
    setWindowTitle(QStringLiteral("原始功能码 / 报文发送"));
    resize(760, 560);

    auto *layout = new QVBoxLayout(this);

    // ---------- 请求参数 ----------
    auto *reqGroup = new QGroupBox(QStringLiteral("请求"), this);
    auto *form = new QFormLayout(reqGroup);

    m_serverSpin = new QSpinBox(reqGroup);
    m_serverSpin->setRange(0, 247);
    m_serverSpin->setValue(1);
    m_serverSpin->setToolTip(QStringLiteral("从站地址（Unit ID）；0 = 广播，广播写不会收到响应"));
    form->addRow(QStringLiteral("从站地址"), m_serverSpin);

    m_funcCombo = new QComboBox(reqGroup);
    m_funcCombo->setEditable(true);
    for (const FuncCodeEntry &e : kFuncCodes)
        m_funcCombo->addItem(QString::fromUtf8(e.label), e.code);
    m_funcCombo->setCurrentIndex(2);   // 默认 03 读保持寄存器
    m_funcCombo->setToolTip(QStringLiteral("可直接从列表选择，也可手动输入十六进制功能码（如 2B）"));
    form->addRow(QStringLiteral("功能码"), m_funcCombo);

    m_payloadEdit = new QLineEdit(reqGroup);
    m_payloadEdit->setPlaceholderText(QStringLiteral("PDU 数据域（十六进制），如：00 00 00 0A"));
    form->addRow(QStringLiteral("数据(PDU)"), m_payloadEdit);

    auto *hint = new QLabel(QStringLiteral(
        "示例：<b>03</b> 读保持寄存器 → <code>起始地址Hi Lo 数量Hi Lo</code>，如 <code>00 00 00 0A</code>；"
        "<b>08</b> 诊断 → <code>子功能Hi Lo 数据Hi Lo</code>；"
        "<b>16</b> 掩码写 → <code>地址Hi Lo ANDHi Lo ORHi Lo</code>；"
        "<b>2B</b> 读设备标识 → <code>0E 01 00</code>。"), reqGroup);
    hint->setWordWrap(true);
    form->addRow(QString(), hint);

    auto *opRow = new QHBoxLayout();
    m_sendBtn = new QPushButton(QStringLiteral("发送"), reqGroup);
    m_loopCheck = new QCheckBox(QStringLiteral("循环发送"), reqGroup);
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
    auto *respGroup = new QGroupBox(QStringLiteral("响应与日志"), this);
    auto *respLayout = new QVBoxLayout(respGroup);
    m_log = new QPlainTextEdit(respGroup);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    respLayout->addWidget(m_log);
    layout->addWidget(respGroup, 1);

    // ---------- 状态与关闭 ----------
    auto *bottom = new QHBoxLayout();
    m_status = new QLabel(QStringLiteral("发送: 0  成功: 0  失败: 0"), this);
    bottom->addWidget(m_status);
    bottom->addStretch();
    auto *clearBtn = new QPushButton(QStringLiteral("清空日志"), this);
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
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

    appendLog(QStringLiteral("提示：带下划线的示例见上方；发送前请先在主界面完成连接。"), false);
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
    m_status->setText(QStringLiteral("发送: %1  成功: %2  失败: %3")
                          .arg(m_sent).arg(m_ok).arg(m_fail));
}

void RawRequestDialog::sendRequest()
{
    if (m_busy)
        return;

    QModbusClient *client = m_clientProvider ? m_clientProvider() : nullptr;
    if (!client || client->state() != QModbusDevice::ConnectedState) {
        appendLog(QStringLiteral("[%1] 未连接设备，请先在主界面连接后再发送。").arg(timestamp()), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    QByteArray payload;
    QString err;
    if (!parseHexBytes(m_payloadEdit->text(), &payload, &err)) {
        appendLog(QStringLiteral("[%1] PDU 解析失败：%2").arg(timestamp(), err), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    const quint8 code = currentFunctionCode();
    if (code == 0) {
        appendLog(QStringLiteral("[%1] 功能码无效，请输入 01~7F 的十六进制值。").arg(timestamp()), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    const int server = m_serverSpin->value();
    QModbusRequest request(static_cast<QModbusPdu::FunctionCode>(code), payload);
    m_inFlight = client->sendRawRequest(request, server);
    if (!m_inFlight) {
        appendLog(QStringLiteral("[%1] 发送失败：%2").arg(timestamp(), client->errorString()), true);
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
    const QString fc = hexByte(currentFunctionCode());

    if (reply->error() != QModbusDevice::NoError) {
        ++m_fail;
        appendLog(QStringLiteral("[%1] 从站 %2  FC %3  → 失败：%4  (%5 ms)")
                      .arg(timestamp())
                      .arg(m_serverSpin->value())
                      .arg(fc, reply->errorString())
                      .arg(ms), true);
    } else {
        ++m_ok;
        const QModbusResponse resp = reply->rawResult();
        const QString hex = QString::fromLatin1(resp.data().toHex(' ')).toUpper();
        QString extra;
        if (resp.isException()) {
            const int ex = static_cast<int>(resp.exceptionCode());
            extra = QStringLiteral("   ← 异常码 %1 (%2)").arg(hexByte(static_cast<quint8>(ex)), exceptionName(ex));
        }
        appendLog(QStringLiteral("[%1] 从站 %2  FC %3  → OK  响应功能码 %4  数据(%5字节)= %6%7  (%8 ms)")
                      .arg(timestamp())
                      .arg(m_serverSpin->value())
                      .arg(fc, hexByte(static_cast<quint8>(resp.functionCode())))
                      .arg(resp.data().size())
                      .arg(hex.isEmpty() ? QStringLiteral("(空)") : hex)
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
    appendLog(QStringLiteral("[%1] 从站 %2  FC %3  → 超时无响应（广播或被静默丢弃）")
                  .arg(timestamp())
                  .arg(m_serverSpin->value())
                  .arg(hexByte(currentFunctionCode())), true);
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
