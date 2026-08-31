#ifndef BATCHTASKMANAGER_H
#define BATCHTASKMANAGER_H

#include <QObject>
#include <QVector>
#include <QTimer>
#include <QDateTime>
#include <QModbusDataUnit>

struct BatchTask {
    int id;
    QString name;
    enum Type { Read, Write } type;
    int serverAddress;
    QModbusDataUnit::RegisterType registerType;
    int startAddress;
    int quantity;
    QVector<quint16> writeValues;
    int delayAfterMs;
    bool enabled;
};

struct BatchTaskResult {
    int taskId;
    QString taskName;
    bool success;
    QString errorMessage;
    QVector<quint16> readValues;
    QDateTime timestamp;
};

class BatchTaskManager : public QObject
{
    Q_OBJECT

public:
    explicit BatchTaskManager(QObject *parent = nullptr);

    int addTask(const BatchTask &task);
    void removeTask(int taskId);
    void updateTask(const BatchTask &task);
    void moveTaskUp(int taskId);
    void moveTaskDown(int taskId);
    QVector<BatchTask> tasks() const;
    void clearTasks();

    void start();
    void stop();
    bool isRunning() const;
    int currentTaskIndex() const;

    bool saveToFile(const QString &filePath) const;
    bool loadFromFile(const QString &filePath);

signals:
    void taskStarted(int taskId);
    void taskCompleted(const BatchTaskResult &result);
    void allTasksCompleted(const QVector<BatchTaskResult> &results);
    void batchStopped();
    void executeReadTask(const BatchTask &task);
    void executeWriteTask(const BatchTask &task);

public slots:
    void onTaskFinished(int taskId, bool success, const QString &error, const QVector<quint16> &values);

private slots:
    void executeNextTask();

private:
    QVector<BatchTask> m_tasks;
    QVector<BatchTaskResult> m_results;
    QTimer *m_delayTimer;
    int m_currentIndex;
    bool m_running;
    int m_nextId;
};

#endif // BATCHTASKMANAGER_H
