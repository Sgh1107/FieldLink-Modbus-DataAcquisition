#include "dashboard.h"
#include <QPaintEvent>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QDateTime>
#include <QtMath>

GaugeWidget::GaugeWidget(QWidget *parent) : QWidget(parent)
{
    m_config.minValue = 0;
    m_config.maxValue = 100;
    m_config.warningThreshold = 70;
    m_config.criticalThreshold = 90;
    m_config.currentValue = 0;
    m_config.normalColor = QColor(0, 209, 178);
    m_config.warningColor = QColor(255, 193, 7);
    m_config.criticalColor = QColor(255, 77, 79);
    setMinimumSize(220, 190);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void GaugeWidget::setConfig(const GaugeConfig &config)
{
    m_config = config;
    update();
}

void GaugeWidget::setValue(double value)
{
    m_config.currentValue = value;
    update();
}

GaugeConfig GaugeWidget::config() const
{
    return m_config;
}

void GaugeWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF outer = rect().adjusted(6, 6, -6, -6);
    QPainterPath cardPath;
    cardPath.addRoundedRect(outer, 14, 14);
    QLinearGradient cardGradient(outer.topLeft(), outer.bottomRight());
    cardGradient.setColorAt(0.0, QColor(35, 45, 63));
    cardGradient.setColorAt(1.0, QColor(17, 24, 36));
    painter.fillPath(cardPath, cardGradient);
    painter.setPen(QPen(QColor(74, 95, 126), 1));
    painter.drawPath(cardPath);

    QRectF titleRect = outer.adjusted(16, 8, -16, -outer.height() + 36);
    QFont titleFont = painter.font();
    titleFont.setPointSize(10);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(226, 236, 248));
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, m_config.name);

    const double range = qMax(1.0, m_config.maxValue - m_config.minValue);
    const double normalized = qBound(0.0, (m_config.currentValue - m_config.minValue) / range, 1.0);

    QColor statusColor = m_config.normalColor;
    QString statusText = QStringLiteral("NORMAL");
    if (m_config.currentValue >= m_config.criticalThreshold) {
        statusColor = m_config.criticalColor;
        statusText = QStringLiteral("ALARM");
    } else if (m_config.currentValue >= m_config.warningThreshold) {
        statusColor = m_config.warningColor;
        statusText = QStringLiteral("WARN");
    }

    QRectF badgeRect(outer.right() - 82, outer.top() + 10, 64, 22);
    QPainterPath badgePath;
    badgePath.addRoundedRect(badgeRect, 11, 11);
    painter.fillPath(badgePath, QColor(statusColor.red(), statusColor.green(), statusColor.blue(), 38));
    painter.setPen(QPen(statusColor, 1));
    painter.drawPath(badgePath);
    QFont badgeFont = painter.font();
    badgeFont.setPointSize(7);
    badgeFont.setBold(true);
    painter.setFont(badgeFont);
    painter.drawText(badgeRect, Qt::AlignCenter, statusText);

    const int side = qMin(static_cast<int>(outer.width() - 32), static_cast<int>(outer.height() - 62));
    QRectF gaugeRect(outer.center().x() - side / 2.0, outer.top() + 38, side, side);
    gaugeRect.adjust(10, 10, -10, -10);

    const QPointF center = gaugeRect.center();
    const double radius = gaugeRect.width() / 2.0;
    const int startAngle = 210;
    const int spanAngle = -240;

    painter.setPen(QPen(QColor(63, 78, 101), 12, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(gaugeRect, startAngle * 16, spanAngle * 16);

    painter.setPen(QPen(statusColor, 12, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(gaugeRect, startAngle * 16, static_cast<int>(spanAngle * normalized * 16));

    painter.setPen(QPen(QColor(105, 127, 158), 1));
    for (int i = 0; i <= 10; ++i) {
        const double angleDeg = startAngle + (spanAngle / 10.0) * i;
        const double rad = qDegreesToRadians(angleDeg);
        const QPointF p1(center.x() + qCos(rad) * (radius - 18), center.y() - qSin(rad) * (radius - 18));
        const QPointF p2(center.x() + qCos(rad) * (radius - 8), center.y() - qSin(rad) * (radius - 8));
        painter.drawLine(p1, p2);
    }

    const double needleAngleDeg = startAngle + spanAngle * normalized;
    const double needleRad = qDegreesToRadians(needleAngleDeg);
    const QPointF needleEnd(center.x() + qCos(needleRad) * (radius - 34),
                            center.y() - qSin(needleRad) * (radius - 34));
    painter.setPen(QPen(QColor(232, 240, 250), 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(center, needleEnd);

    QRadialGradient knobGradient(center, 14);
    knobGradient.setColorAt(0.0, QColor(255, 255, 255));
    knobGradient.setColorAt(1.0, statusColor);
    painter.setBrush(knobGradient);
    painter.setPen(QPen(QColor(18, 24, 34), 2));
    painter.drawEllipse(center, 8, 8);

    QFont valueFont = painter.font();
    valueFont.setPointSize(18);
    valueFont.setBold(true);
    painter.setFont(valueFont);
    painter.setPen(QColor(248, 250, 252));
    QRectF valueRect(outer.left() + 12, outer.bottom() - 62, outer.width() - 24, 30);
    painter.drawText(valueRect, Qt::AlignCenter, QString::number(m_config.currentValue, 'f', 1));

    QFont unitFont = painter.font();
    unitFont.setPointSize(9);
    unitFont.setBold(false);
    painter.setFont(unitFont);
    painter.setPen(QColor(148, 166, 190));
    QRectF unitRect(outer.left() + 12, outer.bottom() - 36, outer.width() - 24, 18);
    painter.drawText(unitRect, Qt::AlignCenter, m_config.unit);

    QFont scaleFont = painter.font();
    scaleFont.setPointSize(7);
    painter.setFont(scaleFont);
    painter.setPen(QColor(124, 144, 172));
    painter.drawText(QRectF(outer.left() + 18, outer.bottom() - 20, 90, 14),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("MIN %1").arg(QString::number(m_config.minValue, 'f', 0)));
    painter.drawText(QRectF(outer.right() - 108, outer.bottom() - 20, 90, 14),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("MAX %1").arg(QString::number(m_config.maxValue, 'f', 0)));
}

Dashboard::Dashboard(QWidget *parent)
    : QWidget(parent)
    , m_columns(2)
{
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(16, 14, 16, 14);
    m_mainLayout->setSpacing(12);

    m_statusLayout = new QHBoxLayout();
    m_statusLayout->setSpacing(10);
    m_connectionStatus = createStatusCard(QStringLiteral("设备状态"), QStringLiteral("DISCONNECTED"), QColor(255, 77, 79));
    m_pollingStatus = createStatusCard(QStringLiteral("轮询状态"), QStringLiteral("STOPPED"), QColor(148, 166, 190));
    m_updateTimeStatus = createStatusCard(QStringLiteral("最近更新时间"), QStringLiteral("--"), QColor(0, 209, 178));
    m_serverStatus = createStatusCard(QStringLiteral("从站地址"), QStringLiteral("1"), QColor(64, 169, 255));
    m_alarmStatus = createStatusCard(QStringLiteral("报警数量"), QStringLiteral("0"), QColor(0, 209, 178));
    m_statusLayout->addWidget(m_connectionStatus);
    m_statusLayout->addWidget(m_pollingStatus);
    m_statusLayout->addWidget(m_updateTimeStatus);
    m_statusLayout->addWidget(m_serverStatus);
    m_statusLayout->addWidget(m_alarmStatus);
    m_mainLayout->addLayout(m_statusLayout);

    m_gaugeLayout = new QGridLayout();
    m_gaugeLayout->setSpacing(14);
    m_mainLayout->addLayout(m_gaugeLayout, 1);

    m_bottomLayout = new QHBoxLayout();
    m_bottomLayout->setSpacing(12);

    m_alarmLog = new QPlainTextEdit(this);
    m_alarmLog->setReadOnly(true);
    m_alarmLog->setMaximumBlockCount(200);
    m_alarmLog->setPlaceholderText(QStringLiteral("报警摘要"));

    m_commLog = new QPlainTextEdit(this);
    m_commLog->setReadOnly(true);
    m_commLog->setMaximumBlockCount(300);
    m_commLog->setPlaceholderText(QStringLiteral("通信日志摘要"));

    m_bottomLayout->addWidget(m_alarmLog, 1);
    m_bottomLayout->addWidget(m_commLog, 1);
    m_mainLayout->addLayout(m_bottomLayout, 0);
}

QLabel *Dashboard::createStatusCard(const QString &title, const QString &value, const QColor &color)
{
    QLabel *label = new QLabel(this);
    label->setMinimumHeight(62);
    label->setAlignment(Qt::AlignCenter);
    updateStatusCard(label, title, value, color);
    return label;
}

void Dashboard::updateStatusCard(QLabel *label, const QString &title, const QString &value, const QColor &color)
{
    label->setText(QStringLiteral("%1\n%2").arg(title, value));
}

int Dashboard::addGauge(const GaugeConfig &config)
{
    auto *gauge = new GaugeWidget(this);
    gauge->setConfig(config);
    m_gauges.append(gauge);
    rebuildLayout();
    return m_gauges.size() - 1;
}

void Dashboard::removeGauge(int index)
{
    if (index < 0 || index >= m_gauges.size()) return;
    delete m_gauges[index];
    m_gauges.removeAt(index);
    rebuildLayout();
}

void Dashboard::updateGaugeValue(int index, double value)
{
    if (index >= 0 && index < m_gauges.size())
        m_gauges[index]->setValue(value);
}

void Dashboard::setColumns(int cols)
{
    m_columns = qMax(1, cols);
    rebuildLayout();
}

int Dashboard::gaugeCount() const { return m_gauges.size(); }

void Dashboard::clearAll()
{
    qDeleteAll(m_gauges);
    m_gauges.clear();
    rebuildLayout();
}

void Dashboard::setConnectionStatus(bool connected, const QString &deviceText)
{
    updateStatusCard(m_connectionStatus, QStringLiteral("设备状态"),
                     connected ? QStringLiteral("CONNECTED") : QStringLiteral("DISCONNECTED"),
                     connected ? QColor(0, 209, 178) : QColor(255, 77, 79));
    if (!deviceText.isEmpty())
        appendCommunicationLog(QStringLiteral("设备状态: %1").arg(deviceText));
}

void Dashboard::setPollingStatus(bool running)
{
    updateStatusCard(m_pollingStatus, QStringLiteral("轮询状态"),
                     running ? QStringLiteral("RUNNING") : QStringLiteral("STOPPED"),
                     running ? QColor(0, 209, 178) : QColor(148, 166, 190));
}

void Dashboard::setLastUpdateTime(const QDateTime &time)
{
    updateStatusCard(m_updateTimeStatus, QStringLiteral("最近更新时间"),
                     time.isValid() ? time.toString("HH:mm:ss") : QStringLiteral("--"),
                     QColor(64, 169, 255));
}

void Dashboard::setServerAddress(int address)
{
    updateStatusCard(m_serverStatus, QStringLiteral("从站地址"), QString::number(address), QColor(64, 169, 255));
}

void Dashboard::setAlarmCount(int count)
{
    updateStatusCard(m_alarmStatus, QStringLiteral("报警数量"), QString::number(count),
                     count > 0 ? QColor(255, 77, 79) : QColor(0, 209, 178));
}

void Dashboard::appendCommunicationLog(const QString &message)
{
    m_commLog->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz ") + message);
}

void Dashboard::appendAlarmLog(const QString &message)
{
    m_alarmLog->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss.zzz ") + message);
}

void Dashboard::rebuildLayout()
{
    while (m_gaugeLayout->count() > 0) {
        QLayoutItem *item = m_gaugeLayout->takeAt(0);
        delete item;
    }

    for (int i = 0; i < m_gauges.size(); ++i) {
        const int row = i / m_columns;
        const int col = i % m_columns;
        m_gaugeLayout->addWidget(m_gauges[i], row, col);
        m_gaugeLayout->setColumnStretch(col, 1);
        m_gaugeLayout->setRowStretch(row, 1);
    }
}
