#ifndef REMOTESERVER_H
#define REMOTESERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMap>
#include <QHash>
#include <QList>
#include <QVector>
#include <functional>

class SecurityManager;

class RemoteServer : public QObject
{
    Q_OBJECT

public:
    // 异步应答回调：由 handler 异步完成时调用，把结果回给客户端。
    // 之所以要异步：handler 内部要等 Modbus 应答，若在 socket 的 readyRead 回调里
    // 用嵌套 QEventLoop 等待，会同时带来两个严重问题：
    //   1) 客户端在等待期间断开 → socket 在嵌套循环里被 deleteLater 销毁 →
    //      回到嵌套循环后继续用该 socket 写响应 → 堆 use-after-free（可远程触发）；
    //   2) 嵌套循环期间 GUI 事件循环被接管，界面冻结；且新到达的请求会重入
    //      processRequest 形成递归，任务量大时可能栈溢出。
    typedef std::function<void(const QJsonObject &)> ResponseCallback;
    typedef std::function<void(int, int, int, int, const ResponseCallback &)> ReadHandler;
    typedef std::function<void(int, int, int, const QVector<quint16> &, const ResponseCallback &)> WriteHandler;

    explicit RemoteServer(QObject *parent = nullptr);
    ~RemoteServer();

    bool start(quint16 port = 8080);
    void stop();
    bool isRunning() const;
    quint16 port() const;
    void setRemoteWriteEnabled(bool enabled);
    void setSecurityManager(SecurityManager *securityManager);
    void setStatusProvider(const std::function<QJsonObject()> &provider);
    void setReadHandler(const ReadHandler &handler);
    void setWriteHandler(const WriteHandler &handler);

signals:
    void clientConnected(const QString &address);
    void clientDisconnected(const QString &address);
    void readRequest(int serverAddress, int registerType, int startAddress, int count);
    void writeRequest(int serverAddress, int registerType, int startAddress, const QVector<quint16> &values);
    void statusRequested();

public slots:
    void sendResponse(const QJsonObject &response);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onClientDisconnected();

private:
    void processRequest(QTcpSocket *socket, const QByteArray &data);
    void processBufferedRequests(QTcpSocket *socket);
    void sendJsonResponse(QTcpSocket *socket, int statusCode, const QJsonObject &body);
    void parseHttpRequest(const QByteArray &data, QString &method, QString &path, QMap<QString, QString> &headers, QByteArray &body);
    bool isAuthorized(const QMap<QString, QString> &headers, const QJsonObject &body) const;
    QString statusText(int statusCode) const;
    void closeConnection(QTcpSocket *socket);

    QTcpServer *m_server;
    QList<QTcpSocket*> m_clients;
    // 每个连接各自的接收缓冲：HTTP 请求可能分多包到达（头 + body），
    // 不累积就无法拼出完整请求 —— 会导致请求被丢弃并多发一个 404。
    QHash<QTcpSocket*, QByteArray> m_buffers;
    quint16 m_port;
    QJsonObject m_lastStatus;
    bool m_remoteWriteEnabled;
    SecurityManager *m_securityManager;
    std::function<QJsonObject()> m_statusProvider;
    ReadHandler m_readHandler;
    WriteHandler m_writeHandler;
};

#endif // REMOTESERVER_H
