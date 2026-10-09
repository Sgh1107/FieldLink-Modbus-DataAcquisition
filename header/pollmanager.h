#ifndef POLLMANAGER_H
#define POLLMANAGER_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QModbusDataUnit>

// 标量成员带默认值：轮询任务经文件持久化（saveToFile/loadFromFile），
// 漏设字段会写入未定义值并在下次加载时被沿用。
struct PollTask {
    int id = 0;
    QString name;
    int serverAddress = 1;
    QModbusDataUnit::RegisterType registerType = QModbusDataUnit::HoldingRegisters;
    int startAddress = 0;
    int quantity = 1;
    int intervalMs = 1000;
    bool enabled = true;
    bool alarmEnabled = false;
    double alarmMin = 0.0;
    double alarmMax = 65535.0;
};

class PollManager : public QObject
{
    Q_OBJECT

public:
    explicit PollManager(QObject *parent = nullptr);
    ~PollManager();

    void addTask(const PollTask &task);
    void removeTask(int taskId);
    void updateTask(const PollTask &task);
    void setTaskEnabled(int taskId, bool enabled);
    QVector<PollTask> tasks() const;
    void startAll();
    void stopAll();
    bool isRunning() const;

    // P2 在途请求保护：任务请求发出后置 inFlight，直到执行方回填完成标志；
    // 在途期间该任务的周期 tick 会被跳过，避免慢设备下请求堆积。
    void notifyTaskFinished(int taskId);

signals:
    void pollRequest(const PollTask &task);
    void alarmTriggered(const PollTask &task, double value, const QString &message);

private slots:
    void onTimerTimeout();

private:
    struct TimerEntry {
        QTimer *timer;
        PollTask task;
        bool inFlight = false;   // 该任务的上一条请求是否仍在途
    };
    QVector<TimerEntry> m_timers;
    bool m_running;
};

#endif // POLLMANAGER_H
