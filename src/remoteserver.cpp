#include "remoteserver.h"
#include "securitymanager.h"
#include <QJsonArray>
#include <QHostAddress>

RemoteServer::RemoteServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(0)
    , m_remoteWriteEnabled(false)
    , m_securityManager(nullptr)
{
    connect(m_server, &QTcpServer::newConnection, this, &RemoteServer::onNewConnection);
}

RemoteServer::~RemoteServer()
{
    stop();
}

bool RemoteServer::start(quint16 port)
{
    if (m_server->isListening())
        return true;

    if (m_server->listen(QHostAddress::Any, port)) {
        m_port = m_server->serverPort();
        return true;
    }
    return false;
}

void RemoteServer::stop()
{
    for (QTcpSocket *client : m_clients) {
        client->disconnectFromHost();
    }
    m_clients.clear();
    m_server->close();
    m_port = 0;
}

bool RemoteServer::isRunning() const
{
    return m_server->isListening();
}

quint16 RemoteServer::port() const
{
    return m_port;
}

void RemoteServer::setRemoteWriteEnabled(bool enabled)
{
    m_remoteWriteEnabled = enabled;
}

void RemoteServer::setSecurityManager(SecurityManager *securityManager)
{
    m_securityManager = securityManager;
}

void RemoteServer::setStatusProvider(const std::function<QJsonObject()> &provider)
{
    m_statusProvider = provider;
}

void RemoteServer::setReadHandler(const std::function<QJsonObject(int, int, int, int)> &handler)
{
    m_readHandler = handler;
}

void RemoteServer::setWriteHandler(const std::function<QJsonObject(int, int, int, const QVector<quint16>&)> &handler)
{
    m_writeHandler = handler;
}

void RemoteServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_clients.append(socket);
        connect(socket, &QTcpSocket::readyRead, this, &RemoteServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &RemoteServer::onClientDisconnected);
        emit clientConnected(socket->peerAddress().toString());
    }
}

void RemoteServer::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    QByteArray data = socket->readAll();
    processRequest(socket, data);
}

void RemoteServer::onClientDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    emit clientDisconnected(socket->peerAddress().toString());
    m_clients.removeOne(socket);
    socket->deleteLater();
}

void RemoteServer::parseHttpRequest(const QByteArray &data, QString &method, QString &path, QMap<QString, QString> &headers, QByteArray &body)
{
    QString request = QString::fromUtf8(data);
    QStringList lines = request.split("\r\n");
    if (lines.isEmpty()) return;

    QStringList firstLine = lines[0].split(" ");
    if (firstLine.size() >= 2) {
        method = firstLine[0];
        path = firstLine[1];
    }

    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].isEmpty())
            break;
        const int split = lines[i].indexOf(':');
        if (split > 0)
            headers.insert(lines[i].left(split).trimmed().toLower(), lines[i].mid(split + 1).trimmed());
    }

    int bodyStart = request.indexOf("\r\n\r\n");
    if (bodyStart != -1) {
        body = data.mid(bodyStart + 4);
    }
}

bool RemoteServer::isAuthorized(const QMap<QString, QString> &headers, const QJsonObject &body) const
{
    QString token = headers.value(QStringLiteral("x-api-token"));
    const QString authorization = headers.value(QStringLiteral("authorization"));
    if (token.isEmpty() && authorization.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive))
        token = authorization.mid(7).trimmed();
    if (token.isEmpty())
        token = body.value(QStringLiteral("token")).toString();

    if (m_securityManager)
        return m_securityManager->verifyApiToken(token);
    return false;
}

QString RemoteServer::statusText(int statusCode) const
{
    switch (statusCode) {
    case 200: return QStringLiteral("OK");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 500: return QStringLiteral("Internal Server Error");
    case 503: return QStringLiteral("Service Unavailable");
    default: return QStringLiteral("Unknown");
    }
}

void RemoteServer::processRequest(QTcpSocket *socket, const QByteArray &data)
{
    QString method, path;
    QByteArray body;
    QMap<QString, QString> headers;
    parseHttpRequest(data, method, path, headers, body);

    if (method == "OPTIONS") {
        sendJsonResponse(socket, 200, QJsonObject{{"status", "ok"}});
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = body.isEmpty() ? QJsonDocument(QJsonObject()) : QJsonDocument::fromJson(body, &parseError);
    if (!body.isEmpty() && parseError.error != QJsonParseError::NoError) {
        sendJsonResponse(socket, 400, QJsonObject{{"error", "Invalid JSON body"}});
        return;
    }
    QJsonObject obj = doc.object();

    const bool knownPath = (path == "/api/status" || path == "/api/read" || path == "/api/write");
    if (knownPath && !isAuthorized(headers, obj)) {
        if (m_securityManager)
            m_securityManager->audit(QStringLiteral("remote"), QStringLiteral("AUTH"), QStringLiteral("denied path=%1").arg(path));
        sendJsonResponse(socket, 401, QJsonObject{{"error", "Unauthorized"}});
        return;
    }
    if (knownPath && m_securityManager)
        m_securityManager->audit(QStringLiteral("remote"), QStringLiteral("AUTH"), QStringLiteral("allowed path=%1").arg(path));

    if (path == "/api/status" && method == "GET") {
        emit statusRequested();
        if (m_statusProvider)
            m_lastStatus = m_statusProvider();
        sendJsonResponse(socket, 200, m_lastStatus);
    }
    else if (path == "/api/read" && method == "POST") {
        const int serverAddress = obj["serverAddress"].toInt(1);
        const int registerType = obj["registerType"].toInt(0);
        const int startAddress = obj["startAddress"].toInt(0);
        const int count = obj["count"].toInt(1);
        if (count <= 0) {
            sendJsonResponse(socket, 400, QJsonObject{{"error", "count must be greater than 0"}});
            return;
        }

        emit readRequest(serverAddress, registerType, startAddress, count);
        QJsonObject resp = m_readHandler
            ? m_readHandler(serverAddress, registerType, startAddress, count)
            : QJsonObject{{"status", "request_sent"}};
        sendJsonResponse(socket, resp.value("success").toBool(true) ? 200 : 503, resp);
    }
    else if (path == "/api/write" && method == "POST") {
        const bool writeAllowed = m_securityManager
            ? (m_securityManager->remoteWriteEnabled() && m_securityManager->isWriteAllowed(QStringLiteral("admin"), QStringLiteral("remote write")))
            : m_remoteWriteEnabled;
        if (!writeAllowed) {
            sendJsonResponse(socket, 403, QJsonObject{{"error", "Remote write disabled"}});
            return;
        }

        QVector<quint16> values;
        QJsonArray arr = obj["values"].toArray();
        for (const auto &v : arr)
            values.append(static_cast<quint16>(v.toInt()));
        if (values.isEmpty()) {
            sendJsonResponse(socket, 400, QJsonObject{{"error", "values must not be empty"}});
            return;
        }

        const int serverAddress = obj["serverAddress"].toInt(1);
        const int registerType = obj["registerType"].toInt(0);
        const int startAddress = obj["startAddress"].toInt(0);
        emit writeRequest(serverAddress, registerType, startAddress, values);
        if (m_securityManager)
            m_securityManager->audit(QStringLiteral("remote"), QStringLiteral("REMOTE_WRITE"), QStringLiteral("server=%1 type=%2 addr=%3 count=%4").arg(serverAddress).arg(registerType).arg(startAddress).arg(values.size()));
        QJsonObject resp = m_writeHandler
            ? m_writeHandler(serverAddress, registerType, startAddress, values)
            : QJsonObject{{"status", "request_sent"}};
        sendJsonResponse(socket, resp.value("success").toBool(true) ? 200 : 503, resp);
    }
    else if (knownPath) {
        sendJsonResponse(socket, 405, QJsonObject{{"error", "Method Not Allowed"}});
    }
    else {
        QJsonObject resp;
        resp["error"] = "Not Found";
        resp["endpoints"] = QJsonArray({"/api/status", "/api/read", "/api/write"});
        sendJsonResponse(socket, 404, resp);
    }
}

void RemoteServer::sendJsonResponse(QTcpSocket *socket, int statusCode, const QJsonObject &body)
{
    QByteArray jsonData = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString statusTextValue = statusText(statusCode);
    QString response = QString(
        "HTTP/1.1 %1 %2\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %3\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type, X-Api-Token, Authorization\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).arg(statusCode).arg(statusTextValue).arg(jsonData.size());

    socket->write(response.toUtf8());
    socket->write(jsonData);
    socket->flush();
}

void RemoteServer::sendResponse(const QJsonObject &response)
{
    m_lastStatus = response;
}
