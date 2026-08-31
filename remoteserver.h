#ifndef REMOTESERVER_H
#define REMOTESERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMap>
#include <QList>
#include <QVector>
#include <functional>

class SecurityManager;

class RemoteServer : public QObject
{
    Q_OBJECT

public:
    explicit RemoteServer(QObject *parent = nullptr);
    ~RemoteServer();

    bool start(quint16 port = 8080);
    void stop();
    bool isRunning() const;
    quint16 port() const;
    void setRemoteWriteEnabled(bool enabled);
    void setSecurityManager(SecurityManager *securityManager);
    void setStatusProvider(const std::function<QJsonObject()> &provider);
    void setReadHandler(const std::function<QJsonObject(int, int, int, int)> &handler);
    void setWriteHandler(const std::function<QJsonObject(int, int, int, const QVector<quint16>&)> &handler);

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
    void sendJsonResponse(QTcpSocket *socket, int statusCode, const QJsonObject &body);
    void parseHttpRequest(const QByteArray &data, QString &method, QString &path, QMap<QString, QString> &headers, QByteArray &body);
    bool isAuthorized(const QMap<QString, QString> &headers, const QJsonObject &body) const;
    QString statusText(int statusCode) const;

    QTcpServer *m_server;
    QList<QTcpSocket*> m_clients;
    quint16 m_port;
    QJsonObject m_lastStatus;
    bool m_remoteWriteEnabled;
    SecurityManager *m_securityManager;
    std::function<QJsonObject()> m_statusProvider;
    std::function<QJsonObject(int, int, int, int)> m_readHandler;
    std::function<QJsonObject(int, int, int, const QVector<quint16>&)> m_writeHandler;
};

#endif // REMOTESERVER_H
