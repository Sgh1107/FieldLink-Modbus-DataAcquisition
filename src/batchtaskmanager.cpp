#include "batchtaskmanager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>

BatchTaskManager::BatchTaskManager(QObject *parent)
    : QObject(parent)
    , m_delayTimer(new QTimer(this))
    , m_currentIndex(-1)
    , m_running(false)
    , m_nextId(1)
{
    m_delayTimer->setSingleShot(true);
    connect(m_delayTimer, &QTimer::timeout, this, &BatchTaskManager::executeNextTask);
}

int BatchTaskManager::addTask(const BatchTask &task)
{
    BatchTask t = task;
    t.id = m_nextId++;
    m_tasks.append(t);
    return t.id;
}

void BatchTaskManager::removeTask(int taskId)
{
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == taskId) {
            m_tasks.removeAt(i);
            break;
        }
    }
}

void BatchTaskManager::updateTask(const BatchTask &task)
{
    for (auto &t : m_tasks) {
        if (t.id == task.id) {
            t = task;
            break;
        }
    }
}

void BatchTaskManager::moveTaskUp(int taskId)
{
    for (int i = 1; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == taskId) {
            m_tasks.swapItemsAt(i, i - 1);
            break;
        }
    }
}

void BatchTaskManager::moveTaskDown(int taskId)
{
    for (int i = 0; i < m_tasks.size() - 1; ++i) {
        if (m_tasks[i].id == taskId) {
            m_tasks.swapItemsAt(i, i + 1);
            break;
        }
    }
}

QVector<BatchTask> BatchTaskManager::tasks() const
{
    return m_tasks;
}

void BatchTaskManager::clearTasks()
{
    m_tasks.clear();
}

void BatchTaskManager::start()
{
    if (m_running || m_tasks.isEmpty()) return;

    m_running = true;
    m_currentIndex = -1;
    m_results.clear();
    executeNextTask();
}

void BatchTaskManager::stop()
{
    m_running = false;
    m_delayTimer->stop();
    emit batchStopped();
}

bool BatchTaskManager::isRunning() const
{
    return m_running;
}

int BatchTaskManager::currentTaskIndex() const
{
    return m_currentIndex;
}

void BatchTaskManager::executeNextTask()
{
    if (!m_running) return;

    ++m_currentIndex;

    while (m_currentIndex < m_tasks.size() && !m_tasks[m_currentIndex].enabled) {
        ++m_currentIndex;
    }

    if (m_currentIndex >= m_tasks.size()) {
        m_running = false;
        emit allTasksCompleted(m_results);
        return;
    }

    const BatchTask &task = m_tasks[m_currentIndex];
    emit taskStarted(task.id);

    if (task.type == BatchTask::Read) {
        emit executeReadTask(task);
    } else {
        emit executeWriteTask(task);
    }
}

void BatchTaskManager::onTaskFinished(int taskId, bool success, const QString &error, const QVector<quint16> &values)
{
    BatchTaskResult result;
    result.taskId = taskId;
    result.success = success;
    result.errorMessage = error;
    result.readValues = values;
    result.timestamp = QDateTime::currentDateTime();

    for (const auto &t : m_tasks) {
        if (t.id == taskId) {
            result.taskName = t.name;
            break;
        }
    }

    m_results.append(result);
    emit taskCompleted(result);

    if (!m_running) return;

    int delayMs = 0;
    if (m_currentIndex >= 0 && m_currentIndex < m_tasks.size()) {
        delayMs = m_tasks[m_currentIndex].delayAfterMs;
    }

    if (delayMs > 0) {
        m_delayTimer->start(delayMs);
    } else {
        executeNextTask();
    }
}

bool BatchTaskManager::saveToFile(const QString &filePath) const
{
    QJsonArray arr;
    for (const auto &task : m_tasks) {
        QJsonObject obj;
        obj["name"] = task.name;
        obj["type"] = static_cast<int>(task.type);
        obj["serverAddress"] = task.serverAddress;
        obj["registerType"] = static_cast<int>(task.registerType);
        obj["startAddress"] = task.startAddress;
        obj["quantity"] = task.quantity;
        obj["delayAfterMs"] = task.delayAfterMs;
        obj["enabled"] = task.enabled;

        QJsonArray valArr;
        for (quint16 v : task.writeValues)
            valArr.append(v);
        obj["writeValues"] = valArr;

        arr.append(obj);
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool BatchTaskManager::loadFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isArray()) return false;

    m_tasks.clear();
    QJsonArray arr = doc.array();
    for (const auto &val : arr) {
        QJsonObject obj = val.toObject();
        BatchTask task;
        task.id = m_nextId++;
        task.name = obj["name"].toString();
        task.type = static_cast<BatchTask::Type>(obj["type"].toInt());
        task.serverAddress = obj["serverAddress"].toInt();
        task.registerType = static_cast<QModbusDataUnit::RegisterType>(obj["registerType"].toInt());
        task.startAddress = obj["startAddress"].toInt();
        task.quantity = obj["quantity"].toInt();
        task.delayAfterMs = obj["delayAfterMs"].toInt();
        task.enabled = obj["enabled"].toBool(true);

        QJsonArray valArr = obj["writeValues"].toArray();
        for (const auto &v : valArr)
            task.writeValues.append(static_cast<quint16>(v.toInt()));

        m_tasks.append(task);
    }
    return true;
}
