#ifndef REALTIMECHART_H
#define REALTIMECHART_H

#include <QWidget>
#include <QVector>
#include <QPainter>
#include <QTimer>
#include <QDateTime>

struct ChartDataPoint {
    QDateTime timestamp;
    double value;
};

struct ChartSeries {
    QString name;
    QColor color;
    QVector<ChartDataPoint> data;
    int maxPoints;
};

class RealTimeChart : public QWidget
{
    Q_OBJECT

public:
    explicit RealTimeChart(QWidget *parent = nullptr);

    int addSeries(const QString &name, const QColor &color, int maxPoints = 100);
    void addDataPoint(int seriesIndex, double value);
    void clearSeries(int seriesIndex);
    void clearAll();
    void setYRange(double min, double max);
    void setAutoScale(bool enabled);
    void setTitle(const QString &title);
    void setShowLegend(bool show);
    void setGridVisible(bool visible);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawGrid(QPainter &painter, const QRect &chartArea);
    void drawAxes(QPainter &painter, const QRect &chartArea);
    void drawSeries(QPainter &painter, const QRect &chartArea);
    void drawLegend(QPainter &painter, const QRect &chartArea);
    void updateYRange();

    QVector<ChartSeries> m_series;
    QString m_title;
    double m_yMin, m_yMax;
    bool m_autoScale;
    bool m_showLegend;
    bool m_showGrid;
    int m_marginLeft, m_marginRight, m_marginTop, m_marginBottom;
};

#endif // REALTIMECHART_H
