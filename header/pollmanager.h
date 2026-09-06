#ifndef POLLMANAGER_H
#define POLLMANAGER_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QModbusDataUnit>

struct PollTask {
    int id;
    QString name;
    int serverAddress;
    QModbusDataUnit::RegisterType registerType;
    int startAddress;
    int quantity;
    int intervalMs;
    bool enabled;
    bool alarmEnabled;
    double alarmMin;
    double alarmMax;
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
