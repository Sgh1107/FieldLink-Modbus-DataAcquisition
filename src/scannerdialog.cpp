#include "scannerdialog.h"
#include "pollmanager.h"
#include "modbusdiagnostics.h"

#include <QModbusClient>
#include <QCoreApplication>
#include <QModbusReply>
#include <QModbusRequest>
#include <QModbusResponse>
#include <QModbusDataUnit>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTableWidget>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QTableWidgetItem>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QStringList>

ScannerDialog::ScannerDialog(std::function<QModbusClient *()> clientProvider,
                             PollManager *pollManager,
                             QWidget *parent)
    : QDialog(parent)
    , m_clientProvider(std::move(clientProvider))
    , m_pollManager(pollManager)
{
    setWindowTitle(tr("Modbus Bus Scanner"));
    resize(820, 620);

    auto *layout = new QVBoxLayout(this);

    // ---------- 扫描参数 ----------
    auto *cfgGroup = new QGroupBox(tr("Scan parameters"), this);
    auto *form = new QFormLayout(cfgGroup);

    auto *rangeRow = new QHBoxLayout();
    m_unitStartSpin = new QSpinBox(cfgGroup);
    m_unitStartSpin->setRange(1, 247);
    m_unitStartSpin->setValue(1);
    m_unitEndSpin = new QSpinBox(cfgGroup);
    m_unitEndSpin->setRange(1, 247);
    m_unitEndSpin->setValue(16);
    rangeRow->addWidget(m_unitStartSpin);
    rangeRow->addWidget(new QLabel(QStringLiteral("~"), cfgGroup));
    rangeRow->addWidget(m_unitEndSpin);
    rangeRow->addStretch();
    form->addRow(tr("Slave address range"), rangeRow);

    m_attemptsSpin = new QSpinBox(cfgGroup);
    m_attemptsSpin->setRange(1, 10);
    m_attemptsSpin->setValue(1);
    m_attemptsSpin->setToolTip(tr("Max attempts per address (first response marks it online)"));
    form->addRow(tr("Attempts per address"), m_attemptsSpin);

    m_funcCombo = new QComboBox(cfgGroup);
    const ModbusDiagnostics::ProbeOption *opts = ModbusDiagnostics::probeOptions();
    for (int i = 0; i < ModbusDiagnostics::probeOptionCount(); ++i)
        m_funcCombo->addItem(QCoreApplication::translate("ModbusDiagnostics", opts[i].label), opts[i].code);
    m_funcCombo->setCurrentIndex(0);
    form->addRow(tr("Probe function code"), m_funcCombo);

    auto *addrRow = new QHBoxLayout();
    m_addrSpin = new QSpinBox(cfgGroup);
    m_addrSpin->setRange(0, 65535);
    m_addrSpin->setValue(0);
    m_countSpin = new QSpinBox(cfgGroup);
    m_countSpin->setRange(1, 125);
    m_countSpin->setValue(1);
    addrRow->addWidget(new QLabel(tr("Start address"), cfgGroup));
    addrRow->addWidget(m_addrSpin);
    addrRow->addWidget(new QLabel(tr("Count"), cfgGroup));
    addrRow->addWidget(m_countSpin);
    addrRow->addStretch();
    form->addRow(tr("Probe target"), addrRow);

    auto *btnRow = new QHBoxLayout();
    m_startBtn = new QPushButton(tr("Start Scan"), cfgGroup);
    m_stopBtn = new QPushButton(tr("Stop"), cfgGroup);
    m_stopBtn->setEnabled(false);
    btnRow->addWidget(m_startBtn);
    btnRow->addWidget(m_stopBtn);
    btnRow->addStretch();
    form->addRow(QString(), btnRow);

    layout->addWidget(cfgGroup);

    // ---------- 结果 ----------
    auto *resGroup = new QGroupBox(tr("Scan results (online slaves only)"), this);
    auto *resLayout = new QVBoxLayout(resGroup);
    m_table = new QTableWidget(resGroup);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({ tr("Slave address"), tr("Status"),
                                         tr("Elapsed (ms)"), tr("Note") });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resLayout->addWidget(m_table);
    layout->addWidget(resGroup, 1);

    // ---------- 底部 ----------
    auto *bottom = new QHBoxLayout();
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_status = new QLabel(tr("Ready"), this);
    bottom->addWidget(m_progress, 1);
    bottom->addWidget(m_status);
    layout->addLayout(bottom);

    auto *actionRow = new QHBoxLayout();
    m_taskBtn = new QPushButton(tr("Create Poll Task for Selected"), this);
    m_copyBtn = new QPushButton(tr("Copy Results"), this);
    auto *clearBtn = new QPushButton(tr("Clear"), this);
    auto *closeBtn = new QPushButton(tr("Close"), this);
    actionRow->addWidget(m_taskBtn);
    actionRow->addWidget(m_copyBtn);
    actionRow->addStretch();
    actionRow->addWidget(clearBtn);
    actionRow->addWidget(closeBtn);
    layout->addLayout(actionRow);

    m_guardTimer = new QTimer(this);
    m_guardTimer->setSingleShot(true);

    connect(m_startBtn, &QPushButton::clicked, this, &ScannerDialog::startScan);
    connect(m_stopBtn, &QPushButton::clicked, this, &ScannerDialog::stopScan);
    connect(m_guardTimer, &QTimer::timeout, this, &ScannerDialog::onGuardTimeout);
    connect(m_taskBtn, &QPushButton::clicked, this, &ScannerDialog::createPollTaskForSelection);
    connect(m_copyBtn, &QPushButton::clicked, this, &ScannerDialog::copyResults);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        m_table->setRowCount(0);
        m_found = 0;
        m_status->setText(tr("Results cleared"));
    });
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        if (m_running) stopScan();
        accept();
    });
}

ScannerDialog::~ScannerDialog()
{
    if (m_guardTimer) m_guardTimer->stop();
}

void ScannerDialog::startScan()
{
    QModbusClient *client = m_clientProvider ? m_clientProvider() : nullptr;
    if (!client || client->state() != QModbusDevice::ConnectedState) {
        QMessageBox::warning(this, tr("Cannot Scan"),
                             tr("Connect a device (TCP/RTU) from the main window before scanning."));
        return;
    }
    int totalUnits = 0;
    QString rangeError;
    if (!ModbusDiagnostics::validateScanRange(m_unitStartSpin->value(),
                                              m_unitEndSpin->value(), &totalUnits, &rangeError)) {
        QMessageBox::warning(this, tr("Cannot Scan"), rangeError);
        return;
    }

    m_running = true;
    m_table->setRowCount(0);
    m_found = 0;
    m_scanned = 0;
    m_currentUnit = m_unitStartSpin->value();
    m_currentAttempt = 0;
    m_total = totalUnits;

    m_startBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_progress->setRange(0, qMax(1, m_total));
    m_progress->setValue(0);

    sendNextProbe();
}

void ScannerDialog::stopScan()
{
    if (!m_running)
        return;
    m_running = false;
    m_guardTimer->stop();
    if (m_inFlight) {
        m_inFlight->deleteLater();
        m_inFlight = nullptr;
    }
    m_startBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_status->setText(tr("Stopped; found %1 online slave(s)").arg(m_found));
}

void ScannerDialog::sendNextProbe()
{
    if (!m_running)
        return;

    if (m_currentUnit > m_unitEndSpin->value()) {
        finishScan();
        return;
    }

    QModbusClient *client = m_clientProvider ? m_clientProvider() : nullptr;
    if (!client || client->state() != QModbusDevice::ConnectedState) {
        stopScan();
        m_status->setText(tr("Connection lost; scan aborted"));
        return;
    }

    const int code = m_funcCombo->currentData().toInt();
    QModbusRequest request(static_cast<QModbusPdu::FunctionCode>(code & 0xFF),
                           ModbusDiagnostics::readPayload(m_addrSpin->value(), m_countSpin->value()));
    m_inFlight = client->sendRawRequest(request, m_currentUnit);
    if (!m_inFlight) {
        // 发送失败直接跳过该地址
        ++m_scanned;
        ++m_currentUnit;
        m_currentAttempt = 0;
        updateProgress();
        QTimer::singleShot(0, this, &ScannerDialog::sendNextProbe);
        return;
    }

    m_elapsed.restart();
    connect(m_inFlight, &QModbusReply::finished, this, &ScannerDialog::onReplyFinished);
    const int guard = client->timeout() * qMax(1, client->numberOfRetries() + 1) + 500;
    m_guardTimer->start(qMax(500, guard));
}

void ScannerDialog::onReplyFinished()
{
    m_guardTimer->stop();
    if (!m_inFlight)
        return;   // 已由兜底定时器处理

    QModbusReply *reply = m_inFlight;
    m_inFlight = nullptr;
    const int ms = static_cast<int>(m_elapsed.elapsed());

    const bool ok = (reply->error() == QModbusDevice::NoError)
                    && !reply->rawResult().isException();

    if (ok) {
        QString note = tr("Response OK");
        const QModbusResponse resp = reply->rawResult();
        const QByteArray data = resp.data();
        const int probeCode = m_funcCombo->currentData().toInt();
        if (data.size() >= 4 && (probeCode == 0x03 || probeCode == 0x04)) {
            // 寄存器读响应：byte0=字节数，byte1.. 为寄存器大端数据
            const int first = (static_cast<quint8>(data.at(2)) << 8) | static_cast<quint8>(data.at(3));
            note = tr("First register value = %1").arg(first);
        }
        appendFound(m_currentUnit, ms, note);
        ++m_scanned;
        ++m_currentUnit;
        m_currentAttempt = 0;
        updateProgress();
        reply->deleteLater();
        QTimer::singleShot(0, this, &ScannerDialog::sendNextProbe);
        return;
    }

    // 失败：重试 → 或跳过该地址
    reply->deleteLater();
    ++m_currentAttempt;
    if (m_currentAttempt < m_attemptsSpin->value()) {
        QTimer::singleShot(0, this, &ScannerDialog::sendNextProbe);
        return;
    }
    ++m_scanned;
    ++m_currentUnit;
    m_currentAttempt = 0;
    updateProgress();
    QTimer::singleShot(0, this, &ScannerDialog::sendNextProbe);
}

void ScannerDialog::onGuardTimeout()
{
    if (!m_inFlight)
        return;
    QModbusReply *reply = m_inFlight;
    m_inFlight = nullptr;
    reply->deleteLater();

    ++m_currentAttempt;
    if (m_currentAttempt < m_attemptsSpin->value()) {
        QTimer::singleShot(0, this, &ScannerDialog::sendNextProbe);
        return;
    }
    ++m_scanned;
    ++m_currentUnit;
    m_currentAttempt = 0;
    updateProgress();
    QTimer::singleShot(0, this, &ScannerDialog::sendNextProbe);
}

void ScannerDialog::appendFound(int unit, int elapsedMs, const QString &note)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(QString::number(unit)));
    m_table->setItem(row, 1, new QTableWidgetItem(tr("Online")));
    m_table->setItem(row, 2, new QTableWidgetItem(QString::number(elapsedMs)));
    m_table->setItem(row, 3, new QTableWidgetItem(note));
    ++m_found;
}

void ScannerDialog::updateProgress()
{
    // 进度条范围是「地址个数」，直接按计数设置并夹到 [0, total]
    m_progress->setValue(qBound(0, m_scanned, m_total));
    m_status->setText(ModbusDiagnostics::scanProgressText(m_scanned, m_total, m_found));
}

void ScannerDialog::finishScan()
{
    m_running = false;
    m_startBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_progress->setValue(m_total);
    m_status->setText(tr("Scan complete; found %1 online slave(s)").arg(m_found));
}

void ScannerDialog::createPollTaskForSelection()
{
    if (!m_pollManager) {
        QMessageBox::information(this, tr("Unavailable"), tr("Poll manager is not initialized."));
        return;
    }
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("No selection"), tr("Select one or more rows in the results table first."));
        return;
    }

    int added = 0;
    for (const QModelIndex &index : rows) {
        const int unit = m_table->item(index.row(), 0)->text().toInt();
        // 任务参数（寄存器表类型、id、名称等）统一由公共逻辑生成，避免界面层重复推导
        const ModbusDiagnostics::PollTaskSeed seed = ModbusDiagnostics::makePollTaskSeed(
            unit, m_funcCombo->currentData().toInt(),
            m_addrSpin->value(), m_countSpin->value());
        PollTask task;
        task.id = seed.id;
        task.name = seed.name;
        task.serverAddress = seed.serverAddress;
        task.registerType = seed.registerType;
        task.startAddress = seed.startAddress;
        task.quantity = seed.quantity;
        task.intervalMs = seed.intervalMs;
        task.enabled = true;
        task.deviceId = 0;          // 扫描器使用主连接
        task.fromPoints = false;    // 人工任务，点表重建时保留
        m_pollManager->removeTask(task.id);
        m_pollManager->addTask(task);
        ++added;
    }
    m_status->setText(tr("Created poll tasks for %1 slave(s)").arg(added));
}

void ScannerDialog::copyResults()
{
    QStringList lines;
    lines << tr("Slave address\tStatus\tElapsed (ms)\tNote");
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < m_table->columnCount(); ++col) {
            QTableWidgetItem *item = m_table->item(row, col);
            cells << (item ? item->text() : QString());
        }
        lines << cells.join(QLatin1Char('\t'));
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    m_status->setText(tr("Results copied to clipboard"));
}
