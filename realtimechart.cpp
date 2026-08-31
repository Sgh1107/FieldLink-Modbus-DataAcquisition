#include "realtimechart.h"
#include <QPaintEvent>
#include <QDateTime>
#include <cmath>

RealTimeChart::RealTimeChart(QWidget *parent)
    : QWidget(parent)
    , m_yMin(0), m_yMax(100)
    , m_autoScale(true)
    , m_showLegend(true)
    , m_showGrid(true)
    , m_marginLeft(60), m_marginRight(20)
    , m_marginTop(30), m_marginBottom(30)
{
    setMinimumSize(300, 200);
}

int RealTimeChart::addSeries(const QString &name, const QColor &color, int maxPoints)
{
    ChartSeries series;
    series.name = name;
    series.color = color;
    series.maxPoints = maxPoints;
    m_series.append(series);
    return m_series.size() - 1;
}

void RealTimeChart::addDataPoint(int seriesIndex, double value)
{
    if (seriesIndex < 0 || seriesIndex >= m_series.size()) return;

    ChartDataPoint point;
    point.timestamp = QDateTime::currentDateTime();
    point.value = value;

    auto &series = m_series[seriesIndex];
    series.data.append(point);
    while (series.data.size() > series.maxPoints)
        series.data.removeFirst();

    if (m_autoScale) updateYRange();
    update();
}

void RealTimeChart::clearSeries(int seriesIndex)
{
    if (seriesIndex >= 0 && seriesIndex < m_series.size())
        m_series[seriesIndex].data.clear();
    update();
}

void RealTimeChart::clearAll()
{
    for (auto &s : m_series) s.data.clear();
    update();
}

void RealTimeChart::setYRange(double min, double max)
{
    m_yMin = min; m_yMax = max;
    m_autoScale = false;
    update();
}

void RealTimeChart::setAutoScale(bool enabled) { m_autoScale = enabled; update(); }
void RealTimeChart::setTitle(const QString &title) { m_title = title; update(); }
void RealTimeChart::setShowLegend(bool show) { m_showLegend = show; update(); }
void RealTimeChart::setGridVisible(bool visible) { m_showGrid = visible; update(); }

void RealTimeChart::updateYRange()
{
    double minVal = 1e18, maxVal = -1e18;
    for (const auto &s : m_series) {
        for (const auto &p : s.data) {
            if (p.value < minVal) minVal = p.value;
            if (p.value > maxVal) maxVal = p.value;
        }
    }
    if (minVal > maxVal) { minVal = 0; maxVal = 100; }
    double margin = (maxVal - minVal) * 0.1;
    if (margin < 1.0) margin = 1.0;
    m_yMin = minVal - margin;
    m_yMax = maxVal + margin;
}

void RealTimeChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRect chartArea(m_marginLeft, m_marginTop,
                    width() - m_marginLeft - m_marginRight,
                    height() - m_marginTop - m_marginBottom);

    painter.fillRect(rect(), Qt::white);

    if (!m_title.isEmpty()) {
        painter.setPen(Qt::black);
        painter.drawText(QRect(0, 2, width(), m_marginTop), Qt::AlignCenter, m_title);
    }

    if (m_showGrid) drawGrid(painter, chartArea);
    drawAxes(painter, chartArea);
    drawSeries(painter, chartArea);
    if (m_showLegend) drawLegend(painter, chartArea);
}

void RealTimeChart::drawGrid(QPainter &painter, const QRect &area)
{
    painter.setPen(QPen(QColor(220, 220, 220), 1, Qt::DashLine));
    int hLines = 5;
    for (int i = 1; i < hLines; ++i) {
        int y = area.top() + area.height() * i / hLines;
        painter.drawLine(area.left(), y, area.right(), y);
    }
    int vLines = 5;
    for (int i = 1; i < vLines; ++i) {
        int x = area.left() + area.width() * i / vLines;
        painter.drawLine(x, area.top(), x, area.bottom());
    }
}

void RealTimeChart::drawAxes(QPainter &painter, const QRect &area)
{
    painter.setPen(QPen(Qt::black, 1));
    painter.drawRect(area);

    int hLines = 5;
    for (int i = 0; i <= hLines; ++i) {
        int y = area.top() + area.height() * i / hLines;
        double val = m_yMax - (m_yMax - m_yMin) * i / hLines;
        painter.drawText(QRect(0, y - 10, m_marginLeft - 5, 20), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(val, 'f', 1));
    }
}

void RealTimeChart::drawSeries(QPainter &painter, const QRect &area)
{
    double yRange = m_yMax - m_yMin;
    if (yRange == 0) yRange = 1;

    for (const auto &series : m_series) {
        if (series.data.size() < 2) continue;

        painter.setPen(QPen(series.color, 2));
        int count = series.data.size();
        double xStep = (double)area.width() / (series.maxPoints - 1);

        int startIdx = qMax(0, count - series.maxPoints);
        QVector<QPointF> points;
        for (int i = startIdx; i < count; ++i) {
            double x = area.left() + (i - startIdx) * xStep;
            double y = area.bottom() - (series.data[i].value - m_yMin) / yRange * area.height();
            points.append(QPointF(x, y));
        }

        for (int i = 1; i < points.size(); ++i) {
            painter.drawLine(points[i - 1], points[i]);
        }
    }
}

void RealTimeChart::drawLegend(QPainter &painter, const QRect &area)
{
    int x = area.right() - 120;
    int y = area.top() + 5;
    for (const auto &series : m_series) {
        painter.setPen(QPen(series.color, 2));
        painter.drawLine(x, y + 7, x + 20, y + 7);
        painter.setPen(Qt::black);
        painter.drawText(x + 25, y + 12, series.name);
        y += 18;
    }
}
