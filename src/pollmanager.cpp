#include "pollmanager.h"
#include <QVariant>

PollManager::PollManager(QObject *parent)
    : QObject(parent)
    , m_running(false)
{
}

PollManager::~PollManager()
{
    stopAll();
    for (auto &entry : m_timers) {
        delete entry.timer;
        entry.timer = nullptr;
    }
    m_timers.clear();
}

void PollManager::addTask(const PollTask &task)
{
    TimerEntry entry;
    entry.task = task;
    entry.timer = new QTimer(this);
    entry.timer->setInterval(task.intervalMs);
    entry.timer->setProperty("taskId", QVariant(task.id));
    connect(entry.timer, &QTimer::timeout, this, &PollManager::onTimerTimeout);
    m_timers.append(entry);

    if (m_running && task.enabled) {
        entry.timer->start();
    }
}

void PollManager::removeTask(int taskId)
{
    for (int i = 0; i < m_timers.size(); ++i) {
        if (m_timers[i].task.id == taskId) {
            m_timers[i].timer->stop();
            delete m_timers[i].timer;
            m_timers.removeAt(i);
            break;
        }
    }
}

void PollManager::updateTask(const PollTask &task)
{
    for (auto &entry : m_timers) {
        if (entry.task.id == task.id) {
            entry.task = task;
            entry.timer->setInterval(task.intervalMs);
            if (m_running) {
                if (task.enabled && !entry.timer->isActive()) {
                    entry.timer->start();
                } else if (!task.enabled && entry.timer->isActive()) {
                    entry.timer->stop();
                }
            }
            break;
        }
    }
}

void PollManager::setTaskEnabled(int taskId, bool enabled)
{
    for (auto &entry : m_timers) {
        if (entry.task.id == taskId) {
            entry.task.enabled = enabled;
            if (m_running) {
                if (enabled) entry.timer->start();
                else entry.timer->stop();
            }
            break;
        }
    }
}

QVector<PollTask> PollManager::tasks() const
{
    QVector<PollTask> result;
    for (const auto &entry : m_timers) {
        result.append(entry.task);
    }
    return result;
}

void PollManager::startAll()
{
    m_running = true;
    for (auto &entry : m_timers) {
        if (entry.task.enabled) {
            entry.timer->start();
        }
    }
}

void PollManager::stopAll()
{
    m_running = false;
    for (auto &entry : m_timers) {
        entry.timer->stop();
    }
}

bool PollManager::isRunning() const
{
    return m_running;
}

void PollManager::onTimerTimeout()
{
    QTimer *timer = qobject_cast<QTimer*>(sender());
    if (!timer) return;

    int taskId = timer->property("taskId").toInt();
    for (const auto &entry : m_timers) {
        if (entry.task.id == taskId) {
            emit pollRequest(entry.task);
            break;
        }
    }
}
