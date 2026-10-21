#include "remoteserver.h"
#include "securitymanager.h"
#include <QJsonArray>
#include <QHostAddress>
#include <QPointer>

namespace {
// 单个请求允许的最大字节数（头 + body）：防止慢速连接持续灌数据耗尽内存
constexpr int kMaxRequestBytes = 1024 * 1024;   // 1 MB
// Modbus 协议上限：单次读最多 125 个寄存器（位区 2000）
constexpr int kMaxReadRegisters = 125;
constexpr int kMaxReadBits = 2000;
constexpr int kMaxWriteRegisters = 123;
constexpr int kMaxWriteBits = 2000;
} // namespace

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

    // 默认只监听本机回环地址：远程写接口暴露到整个局域网是典型的攻击面，
    // 需要远程访问时应由用户显式选择监听所有网卡。
    if (m_server->listen(QHostAddress::LocalHost, port)) {
        m_port = m_server->serverPort();
        return true;
    }
    return false;
}

void RemoteServer::stop()
{
    // 必须真正删除 socket：它们是 m_server 的子对象，只 disconnectFromHost()
    // 会让 socket 一直存活到 RemoteServer 析构，反复 start/stop 会累积 fd。
    const QList<QTcpSocket*> clients = m_clients;
    m_clients.clear();
    m_buffers.clear();
    for (QTcpSocket *client : clients) {
        client->disconnect(this);
        client->deleteLater();
    }
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

void RemoteServer::setReadHandler(const ReadHandler &handler)
{
    m_readHandler = handler;
}

void RemoteServer::setWriteHandler(const WriteHandler &handler)
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

    // 必须按连接累积缓冲：HTTP 请求的头与 body 可能分多包到达。
    // 之前每次 readAll 后立即解析：第 1 包被当成完整请求（body 不全 → 400），
    // 第 2 包没有请求行 → 404，客户端收到两个响应，真正的业务请求从未执行。
    m_buffers[socket].append(socket->readAll());

    // 缓冲上限：防止客户端持续灌数据导致内存耗尽
    if (m_buffers.value(socket).size() > kMaxRequestBytes) {
        sendJsonResponse(socket, 413, QJsonObject{{"error", "Request too large"}});
        closeConnection(socket);
        m_buffers.remove(socket);
        return;
    }

    processBufferedRequests(socket);
}

// 从缓冲中切出完整请求并逐个处理。
void RemoteServer::processBufferedRequests(QTcpSocket *socket)
{
    // 注意：不能把 m_buffers[socket] 的引用绑定在循环外。
    // processRequest → sendJsonResponse → closeConnection 会关闭连接，
    // 随后的 disconnected 信号会触发 onClientDisconnected → m_buffers.remove(socket)，
    // 该引用随即悬垂（use-after-free）。
    // 因此每轮循环都重新取引用，并在处理后用 QPointer 确认 socket 仍存活。
    QPointer<QTcpSocket> guard(socket);

    while (guard) {
        QByteArray &buffer = m_buffers[socket];
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            break;                       // 头未收全，等待更多数据

        // 解析 Content-Length，决定 body 长度
        int contentLength = 0;
        {
            const QByteArray head = buffer.left(headerEnd);
            const QList<QByteArray> lines = head.split('\n');
            for (const QByteArray &rawLine : lines) {
                QByteArray line = rawLine;
                if (line.endsWith('\r')) line.chop(1);
                const int colon = line.indexOf(':');
                if (colon <= 0)
                    continue;
                const QByteArray key = line.left(colon).trimmed().toLower();
                if (key == "content-length") {
                    bool ok = false;
                    const int v = line.mid(colon + 1).trimmed().toInt(&ok);
                    if (ok && v > 0)
                        contentLength = v;
                    break;
                }
            }
        }
        if (contentLength > kMaxRequestBytes) {
            sendJsonResponse(socket, 413, QJsonObject{{"error", "Request body too large"}});
            closeConnection(socket);
            m_buffers.remove(socket);
            return;
        }

        const int totalNeeded = headerEnd + 4 + contentLength;
        if (buffer.size() < totalNeeded)
            break;                       // body 未收全，等待更多数据

        const QByteArray request = buffer.left(totalNeeded);
        buffer.remove(0, totalNeeded);

        processRequest(socket, request);

        // processRequest 可能已关闭并销毁 socket（QPointer 变空），
        // 此时 m_buffers 中的条目已被 onClientDisconnected 移除，不能再访问。
        if (!guard)
            break;
    }
}

void RemoteServer::onClientDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    emit clientDisconnected(socket->peerAddress().toString());
    m_clients.removeOne(socket);
    m_buffers.remove(socket);
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

    // 剥离 query string：路径是精确白名单匹配，不剥离的话
    // "GET /api/status?x=1" 会匹配不上而返回 404。
    const int qmark = path.indexOf(QLatin1Char('?'));
    if (qmark >= 0)
        path = path.left(qmark);

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

        // 参数白名单校验：Qt 6 的 QModbusDataUnit(type, int, quint16) 第三参是
        // quint16，超范围会被静默截断（count=65536 → 0，2000000000 → 37888），
        // 请求语义与实际执行完全不符。
        if (registerType < 1 || registerType > 4) {
            sendJsonResponse(socket, 400, QJsonObject{{"error", "registerType must be 1..4"}});
            return;
        }
        if (startAddress < 0 || startAddress > 65535) {
            sendJsonResponse(socket, 400, QJsonObject{{"error", "startAddress must be 0..65535"}});
            return;
        }
        const bool isBit = (registerType == 1 || registerType == 2);   // Coils / DiscreteInputs
        const int maxCount = isBit ? kMaxReadBits : kMaxReadRegisters;
        if (count <= 0 || count > maxCount) {
            sendJsonResponse(socket, 400,
                             QJsonObject{{"error", QStringLiteral("count must be 1..%1").arg(maxCount)}});
            return;
        }

        emit readRequest(serverAddress, registerType, startAddress, count);
        // 异步应答：handler 完成后才回调发响应。
        // 用 QPointer 保护 socket —— 等待 Modbus 应答期间客户端可能断开，
        // socket 会被 deleteLater 销毁，此时回调里绝不能再碰它。
        QPointer<QTcpSocket> guard(socket);
        const ResponseCallback finish = [this, guard](const QJsonObject &resp) {
            if (!guard)
                return;     // 客户端已断开：静默丢弃应答
            sendJsonResponse(guard, resp.value("success").toBool(true) ? 200 : 503, resp);
        };
        if (m_readHandler) {
            m_readHandler(serverAddress, registerType, startAddress, count, finish);
        } else {
            finish(QJsonObject{{"status", "request_sent"}});
        }
    }
    else if (path == "/api/write" && method == "POST") {
        // 鉴权：校验"角色权限"而非硬编码操作员名。
        // 之前写死 QStringLiteral("admin")，与实际请求方身份无关 ——
        // 任何持有 API Token 的客户端都能以 admin 身份写寄存器，
        // availablePermissions() 里定义的 remote.write 权限从未被使用。
        bool writeAllowed = m_remoteWriteEnabled;
        if (m_securityManager) {
            writeAllowed = m_securityManager->remoteWriteEnabled()
                           && m_securityManager->hasPermission(QStringLiteral("remote.write"));
            if (!writeAllowed && m_securityManager->currentUser().isEmpty()) {
                // 远程 API 无登录态：退回到"令牌 + 写操作员白名单"模型，
                // 但必须显式记录来源，不能冒充某个具体操作员。
                writeAllowed = m_securityManager->remoteWriteEnabled()
                               && m_securityManager->isWriteAllowed(QStringLiteral("remote"),
                                                                    QStringLiteral("remote write"));
            }
        }
        if (!writeAllowed) {
            sendJsonResponse(socket, 403, QJsonObject{{"error", "Remote write disabled"}});
            return;
        }

        QVector<quint16> values;
        QJsonArray arr = obj["values"].toArray();
        // 逐值校验：越界/非数值必须拒绝，不能静默截断
        // （65536 → 0、-1 → 65535、"abc" → 0 全部静默改写后还返回 success）。
        for (const auto &v : arr) {
            if (!v.isDouble()) {
                sendJsonResponse(socket, 400,
                                 QJsonObject{{"error", "values must all be integers"}});
                return;
            }
            const double d = v.toDouble();
            if (d != static_cast<double>(static_cast<qint64>(d))) {
                sendJsonResponse(socket, 400,
                                 QJsonObject{{"error", "values must all be integers"}});
                return;
            }
            const qint64 iv = static_cast<qint64>(d);
            if (iv < 0 || iv > 65535) {
                sendJsonResponse(socket, 400,
                                 QJsonObject{{"error", "values must be in range 0..65535"}});
                return;
            }
            values.append(static_cast<quint16>(iv));
        }
        if (values.isEmpty()) {
            sendJsonResponse(socket, 400, QJsonObject{{"error", "values must not be empty"}});
            return;
        }

        const int serverAddress = obj["serverAddress"].toInt(1);
        const int registerType = obj["registerType"].toInt(0);
        const int startAddress = obj["startAddress"].toInt(0);
        if (registerType < 1 || registerType > 4) {
            sendJsonResponse(socket, 400, QJsonObject{{"error", "registerType must be 1..4"}});
            return;
        }
        if (startAddress < 0 || startAddress > 65535) {
            sendJsonResponse(socket, 400, QJsonObject{{"error", "startAddress must be 0..65535"}});
            return;
        }
        const bool isBit = (registerType == 1 || registerType == 2);
        const int maxValues = isBit ? kMaxWriteBits : kMaxWriteRegisters;
        if (values.size() > maxValues) {
            sendJsonResponse(socket, 400,
                             QJsonObject{{"error", QStringLiteral("too many values (max %1)").arg(maxValues)}});
            return;
        }

        emit writeRequest(serverAddress, registerType, startAddress, values);
        if (m_securityManager)
            m_securityManager->audit(QStringLiteral("remote"), QStringLiteral("REMOTE_WRITE"), QStringLiteral("server=%1 type=%2 addr=%3 count=%4").arg(serverAddress).arg(registerType).arg(startAddress).arg(values.size()));
        QPointer<QTcpSocket> guard(socket);
        const ResponseCallback finish = [this, guard](const QJsonObject &resp) {
            if (!guard)
                return;
            sendJsonResponse(guard, resp.value("success").toBool(true) ? 200 : 503, resp);
        };
        if (m_writeHandler) {
            m_writeHandler(serverAddress, registerType, startAddress, values, finish);
        } else {
            finish(QJsonObject{{"status", "request_sent"}});
        }
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
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;   // socket 可能已在嵌套事件循环中被销毁（use-after-free 防护）

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

    // 响应声明了 Connection: close → 必须真的关闭，否则连接/句柄无限累积。
    closeConnection(socket);
}

// 响应声明了 Connection: close 就必须真的关闭连接。
// 之前只 write 不 close：HTTP/1.1 客户端会等服务端关闭 → 连接长期占用；
// 攻击者只开连接不发数据即可堆积 QTcpSocket，耗尽句柄（资源耗尽 DoS）。
void RemoteServer::closeConnection(QTcpSocket *socket)
{
    if (!socket)
        return;
    socket->flush();
    socket->disconnectFromHost();
}

void RemoteServer::sendResponse(const QJsonObject &response)
{
    m_lastStatus = response;
}
