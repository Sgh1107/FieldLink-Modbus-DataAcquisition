#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <QWidget>
#include <QVector>
#include <QPainter>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>

struct GaugeConfig {
    QString name;
    QString unit;
    double minValue;
    double maxValue;
    double warningThreshold;
    double criticalThreshold;
    double currentValue;
    QColor normalColor;
    QColor warningColor;
    QColor criticalColor;
};

class GaugeWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GaugeWidget(QWidget *parent = nullptr);
    void setConfig(const GaugeConfig &config);
    void setValue(double value);
    GaugeConfig config() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    GaugeConfig m_config;
};

class Dashboard : public QWidget
{
    Q_OBJECT

public:
    explicit Dashboard(QWidget *parent = nullptr);

    int addGauge(const GaugeConfig &config);
    void removeGauge(int index);
    void updateGaugeValue(int index, double value);
    void setColumns(int cols);
    int gaugeCount() const;
    void clearAll();

    void setConnectionStatus(bool connected, const QString &deviceText);
    void setPollingStatus(bool running);
    void setLastUpdateTime(const QDateTime &time);
    void setServerAddress(int address);
    void setAlarmCount(int count);
    void appendCommunicationLog(const QString &message);
    void appendAlarmLog(const QString &message);

private:
    QLabel *createStatusCard(const QString &title, const QString &value, const QColor &color);
    void rebuildLayout();
    void updateStatusCard(QLabel *label, const QString &title, const QString &value, const QColor &color);

    QVector<GaugeWidget*> m_gauges;
    QVBoxLayout *m_mainLayout;
    QHBoxLayout *m_statusLayout;
    QGridLayout *m_gaugeLayout;
    QHBoxLayout *m_bottomLayout;
    QPlainTextEdit *m_commLog;
    QPlainTextEdit *m_alarmLog;
    QLabel *m_connectionStatus;
    QLabel *m_pollingStatus;
    QLabel *m_updateTimeStatus;
    QLabel *m_serverStatus;
    QLabel *m_alarmStatus;
    int m_columns;
};

#endif // DASHBOARD_H
