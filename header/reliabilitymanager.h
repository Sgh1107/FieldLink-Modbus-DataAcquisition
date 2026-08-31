#ifndef RELIABILITYMANAGER_H
#define RELIABILITYMANAGER_H

#include <QObject>
#include <QTimer>
#include <QDateTime>

class ReliabilityManager : public QObject
{
    Q_OBJECT

public:
    explicit ReliabilityManager(QObject *parent = nullptr);

    void setAutoReconnectEnabled(bool enabled);
    void setHeartbeatEnabled(bool enabled);
    void setReconnectIntervalMs(int intervalMs);
    void setHeartbeatIntervalMs(int intervalMs);
    void setMaxContinuousFailures(int count);

    bool autoReconnectEnabled() const;
    bool heartbeatEnabled() const;
    int continuousFailures() const;
    QDateTime lastSuccessTime() const;
    QDateTime lastFailureTime() const;

    void start();
    void stop();
    void notifySuccess();
    void notifyFailure(const QString &reason);
    void notifyDisconnected();

signals:
    void reconnectRequested();
    void heartbeatRequested();
    void continuousFailureAlarm(int count, const QString &lastReason);
    void recoveryStateChanged(const QString &state);

private slots:
    void onReconnectTimeout();
    void onHeartbeatTimeout();

private:
    QTimer m_reconnectTimer;
    QTimer m_heartbeatTimer;
    bool m_autoReconnectEnabled;
    bool m_heartbeatEnabled;
    int m_reconnectIntervalMs;
    int m_heartbeatIntervalMs;
    int m_maxContinuousFailures;
    int m_continuousFailures;
    QString m_lastFailureReason;
    QDateTime m_lastSuccessTime;
    QDateTime m_lastFailureTime;
};

#endif // RELIABILITYMANAGER_H
