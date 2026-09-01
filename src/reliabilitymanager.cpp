#include "reliabilitymanager.h"

ReliabilityManager::ReliabilityManager(QObject *parent)
    : QObject(parent)
    , m_autoReconnectEnabled(true)
    , m_heartbeatEnabled(true)
    , m_reconnectIntervalMs(3000)
    , m_heartbeatIntervalMs(5000)
    , m_maxContinuousFailures(3)
    , m_continuousFailures(0)
    , m_userIntentConnected(false)
{
    m_reconnectTimer.setSingleShot(false);
    m_heartbeatTimer.setSingleShot(false);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &ReliabilityManager::onReconnectTimeout);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &ReliabilityManager::onHeartbeatTimeout);
}

void ReliabilityManager::setAutoReconnectEnabled(bool enabled)
{
    m_autoReconnectEnabled = enabled;
    if (!enabled)
        m_reconnectTimer.stop();
}

void ReliabilityManager::setHeartbeatEnabled(bool enabled)
{
    m_heartbeatEnabled = enabled;
    if (!enabled)
        m_heartbeatTimer.stop();
}

void ReliabilityManager::setReconnectIntervalMs(int intervalMs)
{
    m_reconnectIntervalMs = qMax(500, intervalMs);
    m_reconnectTimer.setInterval(m_reconnectIntervalMs);
}

void ReliabilityManager::setHeartbeatIntervalMs(int intervalMs)
{
    m_heartbeatIntervalMs = qMax(1000, intervalMs);
    m_heartbeatTimer.setInterval(m_heartbeatIntervalMs);
}

void ReliabilityManager::setMaxContinuousFailures(int count)
{
    m_maxContinuousFailures = qMax(1, count);
}

bool ReliabilityManager::autoReconnectEnabled() const { return m_autoReconnectEnabled; }
bool ReliabilityManager::heartbeatEnabled() const { return m_heartbeatEnabled; }
int ReliabilityManager::continuousFailures() const { return m_continuousFailures; }
bool ReliabilityManager::userIntentConnected() const { return m_userIntentConnected; }
QDateTime ReliabilityManager::lastSuccessTime() const { return m_lastSuccessTime; }
QDateTime ReliabilityManager::lastFailureTime() const { return m_lastFailureTime; }

void ReliabilityManager::setUserIntentConnected(bool connected)
{
    m_userIntentConnected = connected;
    if (!connected) {
        // 用户明确不想要连接（启动阶段/手动断开）：立即停止重连循环
        m_reconnectTimer.stop();
        emit recoveryStateChanged(QStringLiteral("已停止自动重连（用户断开）"));
    }
}

void ReliabilityManager::start()
{
    if (m_autoReconnectEnabled) {
        m_reconnectTimer.setInterval(m_reconnectIntervalMs);
        m_reconnectTimer.start();
    }
    if (m_heartbeatEnabled) {
        m_heartbeatTimer.setInterval(m_heartbeatIntervalMs);
        m_heartbeatTimer.start();
    }
    emit recoveryStateChanged(QStringLiteral("可靠性监控已启动"));
}

void ReliabilityManager::stop()
{
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    emit recoveryStateChanged(QStringLiteral("可靠性监控已停止"));
}

void ReliabilityManager::notifySuccess()
{
    m_continuousFailures = 0;
    m_lastSuccessTime = QDateTime::currentDateTime();
    m_lastFailureReason.clear();
}

void ReliabilityManager::notifyFailure(const QString &reason)
{
    ++m_continuousFailures;
    m_lastFailureReason = reason;
    m_lastFailureTime = QDateTime::currentDateTime();
    if (m_continuousFailures >= m_maxContinuousFailures)
        emit continuousFailureAlarm(m_continuousFailures, m_lastFailureReason);
}

void ReliabilityManager::notifyDisconnected()
{
    notifyFailure(QStringLiteral("设备断线"));
    // 仅在用户希望保持连接时才自动重连（意外断线场景）；
    // 启动阶段 / 手动断开后（意图为 false）不再触发
    if (m_userIntentConnected && m_autoReconnectEnabled && !m_reconnectTimer.isActive())
        m_reconnectTimer.start(m_reconnectIntervalMs);
}

void ReliabilityManager::onReconnectTimeout()
{
    if (m_autoReconnectEnabled && m_userIntentConnected)
        emit reconnectRequested();
}

void ReliabilityManager::onHeartbeatTimeout()
{
    if (m_heartbeatEnabled)
        emit heartbeatRequested();
}
