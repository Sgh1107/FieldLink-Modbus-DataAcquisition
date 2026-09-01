// mcpserver.cpp
// MCP 协议实现：Streamable HTTP 传输 + tools / resources / prompts 三类能力。
//
// 协议要点（Model Context Protocol, 2025-06-18 规范）：
//  - POST /mcp   : JSON-RPC 2.0，application/json 应答；通知以 202 Accepted 空体应答
//  - GET  /mcp   : SSE 长连接，服务端主动推送 notifications/message 等通知
//  - DELETE /mcp : 终止会话
//  - initialize  : 协议版本协商 + 签发 Mcp-Session-Id（宽容模式下不强制携带）
//  - 批量语义    : 兼容请求数组（2025-06-18 起规范已移除批量，此处保持宽容）
//
// 安全要点：Token 鉴权、写入闸门、危险工具 GUI 人工确认、IP 滑动窗口限流、
//           Origin 校验（DNS 重绑定防护）。

#include "mcpserver.h"

#include <QDate>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonParseError>
#include <QUuid>

namespace {

constexpr char kMcpPath[] = "/mcp";
constexpr char kServerName[] = "fieldlink";
constexpr char kServerVersion[] = "2.1.0";
constexpr qint64 kSessionTimeoutMs = 30 * 60 * 1000;   // 会话空闲过期时间：30 分钟

// 服务器支持的 MCP 协议版本（新 -> 旧）
const QStringList kSupportedProtocolVersions = {
    QStringLiteral("2025-06-18"),
    QStringLiteral("2025-03-26"),
    QStringLiteral("2024-11-05"),
};

// JSON-RPC 2.0 标准错误码
constexpr int kParseError     = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams  = -32602;

QString statusText(int code)
{
    switch (code) {
    case 200: return QStringLiteral("OK");
    case 202: return QStringLiteral("Accepted");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 429: return QStringLiteral("Too Many Requests");
    default:  return QStringLiteral("Error");
    }
}

QString extractToken(const QMap<QString, QString> &headers)
{
    QString token = headers.value(QStringLiteral("x-api-token"));
    const QString authorization = headers.value(QStringLiteral("authorization"));
    if (token.isEmpty() && authorization.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive))
        token = authorization.mid(7).trimmed();
    return token;
}

} // namespace

McpServer::McpServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(0)
    , m_writeEnabled(false)
    , m_requireSession(false)
    , m_logLevel(1)          // 默认 info
    , m_rateLimitPerMin(30)
    , m_tools(nullptr)
{
    connect(m_server, &QTcpServer::newConnection, this, &McpServer::onNewConnection);

    // SSE 心跳：保持通知长连接，同时探测死连接
    m_heartbeatTimer.setInterval(15 * 1000);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &McpServer::onHeartbeatTick);

    // 过期会话清理
    m_sessionSweepTimer.setInterval(60 * 1000);
    connect(&m_sessionSweepTimer, &QTimer::timeout, this, &McpServer::onSessionSweep);
}

McpServer::~McpServer()
{
    stop();
}

bool McpServer::start(quint16 port)
{
    if (m_server->isListening())
        return true;
    if (!m_server->listen(QHostAddress::Any, port)) {
        emit logLine(QStringLiteral("MCP 服务监听失败: %1").arg(m_server->errorString()));
        return false;
    }
    m_port = m_server->serverPort();
    m_heartbeatTimer.start();
    m_sessionSweepTimer.start();
    emit runningChanged(true);
    emit logLine(QStringLiteral("MCP 服务已启动 端口:%1 工具:%2 写入:%3 会话:%4 限流:%5/min")
                     .arg(m_port).arg(m_tools ? m_tools->count() : 0)
                     .arg(m_writeEnabled ? QStringLiteral("开启") : QStringLiteral("关闭"))
                     .arg(m_requireSession ? QStringLiteral("强制") : QStringLiteral("宽容"))
                     .arg(m_rateLimitPerMin));
    return true;
}

void McpServer::stop()
{
    if (!m_server->isListening())
        return;
    m_server->close();
    m_heartbeatTimer.stop();
    m_sessionSweepTimer.stop();

    // 关闭 SSE 订阅连接
    for (QTcpSocket *client : qAsConst(m_sseClients))
        client->disconnectFromHost();
    m_sseClients.clear();

    for (QTcpSocket *client : qAsConst(m_clients))
        client->disconnectFromHost();
    m_clients.clear();
    m_requestBuffers.clear();
    m_sessions.clear();
    m_rateWindows.clear();
    m_port = 0;
    emit runningChanged(false);
    emit logLine(QStringLiteral("MCP 服务已停止"));
}

bool McpServer::isRunning() const { return m_server->isListening(); }
quint16 McpServer::port() const { return m_port; }

void McpServer::setToolRegistry(AgentToolRegistry *registry) { m_tools = registry; }
void McpServer::setAuthToken(const QString &token) { m_authToken = token; }
void McpServer::setWriteEnabled(bool enabled) { m_writeEnabled = enabled; }
bool McpServer::writeEnabled() const { return m_writeEnabled; }
void McpServer::setRequireSession(bool enabled) { m_requireSession = enabled; }
bool McpServer::requireSession() const { return m_requireSession; }
void McpServer::setRateLimitPerMinute(int limit) { m_rateLimitPerMin = limit; }
int McpServer::rateLimitPerMinute() const { return m_rateLimitPerMin; }

void McpServer::setDangerousConfirmHandler(
    const std::function<bool(const QString &toolName, const QJsonObject &arguments)> &handler)
{
    m_confirmHandler = handler;
}

void McpServer::setResourceProvider(const std::function<QJsonValue(const QString &uri)> &provider)
{
    m_resourceProvider = provider;
}

// ---------------- 会话 ----------------

QString McpServer::ensureSession()
{
    const QString sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    McpSession session;
    session.lastActive = QDateTime::currentDateTime();
    m_sessions.insert(sessionId, session);
    m_issuedSessionId = sessionId;
    return sessionId;
}

bool McpServer::touchSession(const QString &sessionId)
{
    if (!m_sessions.contains(sessionId))
        return false;
    m_sessions[sessionId].lastActive = QDateTime::currentDateTime();
    return true;
}

void McpServer::endSession(const QString &sessionId)
{
    m_sessions.remove(sessionId);
}

void McpServer::onSessionSweep()
{
    const QDateTime now = QDateTime::currentDateTime();
    QStringList expired;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it.value().lastActive.msecsTo(now) > kSessionTimeoutMs)
            expired.append(it.key());
    }
    for (const QString &id : expired)
        m_sessions.remove(id);
}

// ---------------- 连接与 HTTP 层 ----------------

void McpServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *client = m_server->nextPendingConnection();
        if (!client)
            continue;
        client->setParent(this);
        m_clients.append(client);
        connect(client, &QTcpSocket::readyRead, this, &McpServer::onReadyRead);
        connect(client, &QTcpSocket::disconnected, this, &McpServer::onClientDisconnected);
    }
}

void McpServer::onClientDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;
    m_requestBuffers.remove(socket);
    m_sseClients.removeOne(socket);
    m_clients.removeOne(socket);
    socket->deleteLater();
}

void McpServer::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    // 按连接缓冲，Content-Length 满足后才处理（请求可能与 TCP 分片不对齐）。
    // 提取完成后立刻移出缓冲再处理：危险工具的确认对话框会重入事件循环，
    // 不能在重入期间持有对缓冲 map 的引用。
    m_requestBuffers[socket].append(socket->readAll());
    QByteArray &buffer = m_requestBuffers[socket];

    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;

    const QByteArray head = buffer.left(headerEnd);
    int contentLength = 0;
    const QList<QByteArray> headerLines = head.split('\n');
    for (const QByteArray &line : headerLines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith("Content-Length:"))
            contentLength = trimmed.mid(15).trimmed().toInt();
    }
    if (buffer.size() < headerEnd + 4 + contentLength)
        return;

    const QByteArray request = buffer.left(headerEnd + 4 + contentLength);
    buffer.remove(0, headerEnd + 4 + contentLength);
    processRequest(socket, request);
}

void McpServer::parseHttpRequest(const QByteArray &data, QString &method, QString &path,
                                 QMap<QString, QString> &headers, QByteArray &body) const
{
    const int headerEnd = data.indexOf("\r\n\r\n");
    const QByteArray head = headerEnd >= 0 ? data.left(headerEnd) : data;
    body = headerEnd >= 0 ? data.mid(headerEnd + 4) : QByteArray();

    const QList<QByteArray> lines = head.split('\n');
    if (!lines.isEmpty()) {
        const QList<QByteArray> firstLine = lines.first().trimmed().split(' ');
        if (firstLine.size() >= 2) {
            method = QString::fromLatin1(firstLine[0]);
            path = QString::fromLatin1(firstLine[1]);
        }
    }
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const int colon = line.indexOf(':');
        if (colon > 0)
            headers.insert(QString::fromLatin1(line.left(colon).trimmed()).toLower(),
                           QString::fromUtf8(line.mid(colon + 1).trimmed()));
    }
}

bool McpServer::authorized(const QMap<QString, QString> &headers) const
{
    if (m_authToken.isEmpty())
        return true; // 未配置 Token：不校验（仅建议本机使用）
    return extractToken(headers) == m_authToken;
}

bool McpServer::isRateLimited(const QString &clientKey)
{
    if (m_rateLimitPerMin <= 0)
        return false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<qint64> &window = m_rateWindows[clientKey];

    // 清理 60 秒之外的旧记录
    while (!window.isEmpty() && now - window.first() > 60 * 1000)
        window.removeFirst();

    if (window.size() >= m_rateLimitPerMin)
        return true;

    window.append(now);
    return false;
}

void McpServer::sendHttpResponse(QTcpSocket *socket, int statusCode, const QByteArray &body,
                                 const QByteArray &extraHeaders)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;
    QByteArray response;
    response += QStringLiteral("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText(statusCode)).toUtf8();
    response += "Content-Type: application/json; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += extraHeaders;
    response += "Connection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void McpServer::startSseStream(QTcpSocket *socket, const QMap<QString, QString> &headers)
{
    Q_UNUSED(headers);
    if (!socket)
        return;

    // SSE 响应头：长连接，不断开
    QByteArray response;
    response += "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/event-stream\r\n";
    response += "Cache-Control: no-cache\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "Connection: keep-alive\r\n\r\n";
    response += ": fieldlink mcp notification stream\n\n";   // 注释行，作为流开始标记
    socket->write(response);
    socket->flush();

    if (!m_sseClients.contains(socket))
        m_sseClients.append(socket);
    emit logLine(QStringLiteral("MCP 客户端建立 SSE 通知通道 (%1)").arg(socket->peerAddress().toString()));
}

void McpServer::processRequest(QTcpSocket *socket, const QByteArray &data)
{
    QString method, path;
    QMap<QString, QString> headers;
    QByteArray body;
    parseHttpRequest(data, method, path, headers, body);

    // CORS 预检
    if (method == QStringLiteral("OPTIONS")) {
        if (!socket)
            return;
        QByteArray response;
        response += "HTTP/1.1 200 OK\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n";
        response += "Access-Control-Allow-Headers: Content-Type, Authorization, X-Api-Token, Mcp-Session-Id, MCP-Protocol-Version\r\n";
        response += "Content-Length: 0\r\n";
        response += "Connection: close\r\n\r\n";
        socket->write(response);
        socket->disconnectFromHost();
        return;
    }

    // DNS 重绑定防护：校验 Origin（规范要求）
    const QString origin = headers.value(QStringLiteral("origin"));
    if (!origin.isEmpty()
        && !origin.contains(QStringLiteral("127.0.0.1"))
        && !origin.contains(QStringLiteral("localhost"))
        && !origin.contains(QStringLiteral("[::1]"))) {
        emit logLine(QStringLiteral("MCP 拒绝跨源请求 Origin=%1").arg(origin));
        sendHttpResponse(socket, 403, "{\"error\":\"origin not allowed\"}");
        return;
    }

    // 仅接受 /mcp 端点
    if (path != QLatin1String(kMcpPath)) {
        sendHttpResponse(socket, 404, "{\"error\":\"not found\"}");
        return;
    }

    // 鉴权
    if (!authorized(headers)) {
        emit logLine(QStringLiteral("MCP 鉴权失败"));
        broadcastLogNotification(QStringLiteral("warning"), QStringLiteral("MCP 鉴权失败"));
        sendHttpResponse(socket, 401, "{\"error\":\"unauthorized: missing or invalid API token\"}");
        return;
    }

    // GET /mcp：SSE 通知通道
    if (method == QStringLiteral("GET")) {
        const QString accept = headers.value(QStringLiteral("accept"));
        if (accept.contains(QStringLiteral("text/event-stream"))) {
            startSseStream(socket, headers);
        } else {
            sendHttpResponse(socket, 405,
                             "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"SSE stream requires Accept: text/event-stream\"},\"id\":null}");
        }
        return;
    }

    // DELETE /mcp：终止会话
    if (method == QStringLiteral("DELETE")) {
        const QString sessionId = headers.value(QStringLiteral("mcp-session-id"));
        if (!sessionId.isEmpty() && touchSession(sessionId)) {
            endSession(sessionId);
            sendHttpResponse(socket, 200, "{}");
        } else {
            sendHttpResponse(socket, 404, "{\"error\":\"session not found\"}");
        }
        return;
    }

    if (method != QStringLiteral("POST")) {
        sendHttpResponse(socket, 405,
                         "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"unsupported method\"},\"id\":null}");
        return;
    }

    // IP 滑动窗口限流
    const QString clientKey = socket ? socket->peerAddress().toString() : QStringLiteral("unknown");
    if (isRateLimited(clientKey)) {
        emit logLine(QStringLiteral("MCP 限流触发 client=%1").arg(clientKey));
        broadcastLogNotification(QStringLiteral("warning"), QStringLiteral("MCP 限流触发 client=%1").arg(clientKey));
        sendHttpResponse(socket, 429,
                         "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,\"message\":\"rate limit exceeded\"},\"id\":null}");
        return;
    }

    // 解析 JSON-RPC 载荷
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || doc.isNull()) {
        const QByteArray payload = QJsonDocument(
            errorResponse(QJsonValue(), kParseError, QStringLiteral("Parse error: %1").arg(parseError.errorString())))
            .toJson(QJsonDocument::Compact);
        sendHttpResponse(socket, 400, payload);
        return;
    }

    // 会话处理：宽容模式仅追踪，强制模式校验
    const QString sessionHeader = headers.value(QStringLiteral("mcp-session-id"));
    const bool isInitialize = (doc.isObject()
                               && doc.object().value(QStringLiteral("method")).toString() == QStringLiteral("initialize"));
    if (!sessionHeader.isEmpty()) {
        if (touchSession(sessionHeader))
            m_issuedSessionId.clear();   // 已有会话继续复用，不重复签发
        else if (m_requireSession && !isInitialize) {
            sendHttpResponse(socket, 404,
                             "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"session expired or not found\"},\"id\":null}");
            return;
        }
    } else if (m_requireSession && !isInitialize) {
        sendHttpResponse(socket, 404,
                         "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"missing Mcp-Session-Id\"},\"id\":null}");
        return;
    }
    if (isInitialize)
        ensureSession();

    const QByteArray payload = handleRpcPayload(doc);
    QByteArray extraHeaders;
    if (!m_issuedSessionId.isEmpty()) {
        extraHeaders = "Mcp-Session-Id: " + m_issuedSessionId.toUtf8() + "\r\n";
        m_issuedSessionId.clear();
    }

    if (payload.isEmpty())
        sendHttpResponse(socket, 202, QByteArray(), extraHeaders);   // 通知：202 Accepted 空体
    else
        sendHttpResponse(socket, 200, payload, extraHeaders);
}

// ---------------- JSON-RPC / MCP 层 ----------------

QByteArray McpServer::handleRpcPayload(const QJsonDocument &doc)
{
    // 兼容批量语义：数组则逐条处理（2025-06-18 起规范已移除批量，此处保持宽容）
    if (doc.isArray()) {
        QJsonArray responses;
        const QJsonArray requests = doc.array();
        for (const QJsonValue &item : requests) {
            const QByteArray response = dispatch(item.toObject());
            if (!response.isEmpty())
                responses.append(QJsonDocument::fromJson(response).object());
        }
        if (responses.isEmpty())
            return QByteArray();
        return QJsonDocument(responses).toJson(QJsonDocument::Compact);
    }
    return dispatch(doc.object());
}

QJsonObject McpServer::resultResponse(const QJsonValue &id, const QJsonObject &result) const
{
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result}
    };
}

QJsonObject McpServer::errorResponse(const QJsonValue &id, int code, const QString &message) const
{
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}}}
    };
}

QByteArray McpServer::dispatch(const QJsonObject &request)
{
    const bool isNotification = !request.contains(QStringLiteral("id"));
    const QJsonValue id = request.value(QStringLiteral("id"));
    const QString rpcMethod = request.value(QStringLiteral("method")).toString();
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();

    // 通知：无响应
    if (isNotification && rpcMethod.startsWith(QStringLiteral("notifications/")))
        return QByteArray();

    if (rpcMethod == QStringLiteral("initialize"))
        return QJsonDocument(resultResponse(id, handleInitialize(request))).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("ping"))
        return QJsonDocument(resultResponse(id, QJsonObject())).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("tools/list"))
        return QJsonDocument(resultResponse(id, handleToolsList())).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("tools/call"))
        return QJsonDocument(resultResponse(id, handleToolsCall(params))).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("resources/list"))
        return QJsonDocument(resultResponse(id, handleResourcesList())).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("resources/read"))
        return QJsonDocument(handleResourcesRead(id, params)).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("resources/subscribe")
        || rpcMethod == QStringLiteral("resources/unsubscribe"))
        return QJsonDocument(resultResponse(id, QJsonObject())).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("prompts/list"))
        return QJsonDocument(resultResponse(id, handlePromptsList())).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("prompts/get"))
        return QJsonDocument(resultResponse(id, handlePromptsGet(params))).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("logging/setLevel"))
        return QJsonDocument(resultResponse(id, handleLoggingSetLevel(params))).toJson(QJsonDocument::Compact);

    if (rpcMethod == QStringLiteral("completion/complete")
        || rpcMethod == QStringLiteral("roots/list"))
        return QJsonDocument(resultResponse(id, QJsonObject())).toJson(QJsonDocument::Compact);

    if (isNotification)
        return QByteArray();
    return QJsonDocument(errorResponse(id, kMethodNotFound, QStringLiteral("Method not found: %1").arg(rpcMethod)))
        .toJson(QJsonDocument::Compact);
}

QJsonObject McpServer::handleInitialize(const QJsonObject &request)
{
    const QString clientVersion = request.value(QStringLiteral("params")).toObject()
                                      .value(QStringLiteral("protocolVersion")).toString();

    // 版本协商：客户端版本受支持则原样返回，否则回落到服务器最新支持版本
    QString negotiated = kSupportedProtocolVersions.first();
    if (kSupportedProtocolVersions.contains(clientVersion))
        negotiated = clientVersion;

    emit logLine(QStringLiteral("MCP initialize 客户端协议版本=%1 协商结果=%2").arg(clientVersion, negotiated));
    broadcastLogNotification(QStringLiteral("info"),
                             QStringLiteral("MCP 客户端接入，协议版本 %1").arg(negotiated));

    QJsonObject result;
    result["protocolVersion"] = negotiated;
    result["capabilities"] = QJsonObject{
        {QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), true}}},
        {QStringLiteral("resources"), QJsonObject{{QStringLiteral("subscribe"), false}}},
        {QStringLiteral("prompts"), QJsonObject{{QStringLiteral("listChanged"), false}}},
        {QStringLiteral("logging"), QJsonObject{}}
    };
    result["serverInfo"] = QJsonObject{
        {QStringLiteral("name"), QString::fromLatin1(kServerName)},
        {QStringLiteral("version"), QString::fromLatin1(kServerVersion)}
    };
    result["instructions"] = QStringLiteral(
        "FieldLink 工业级 Modbus 数据采集平台。可通过 tools 读取/写入 Modbus 寄存器、"
        "查询 SQLite 历史数据、管理报警规则与轮询任务；可通过 resources 读取系统状态快照。"
        "写寄存器/改报警规则/轮询控制属于危险操作：需要服务器开启写入闸门，"
        "且每次调用会在 FieldLink 界面上弹出来人工确认框，本机操作员同意后才会执行。");
    return result;
}

QJsonObject McpServer::handleToolsList()
{
    QJsonObject result;
    result["tools"] = m_tools ? m_tools->toolDefinitions() : QJsonArray();
    return result;
}

QJsonObject McpServer::handleToolsCall(const QJsonObject &params)
{
    const QString name = params.value(QStringLiteral("name")).toString();
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    emit logLine(QStringLiteral("MCP tools/call name=%1 写入闸门=%2").arg(name).arg(m_writeEnabled));

    auto errorResult = [](const QString &message) {
        return QJsonObject{
            {QStringLiteral("content"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), message}}}},
            {QStringLiteral("isError"), true}
        };
    };

    if (!m_tools)
        return errorResult(QStringLiteral("tool registry not ready"));

    // 危险工具三层防线：未知工具 -> 写入闸门 -> GUI 人工确认
    const AgentTool *tool = m_tools->tool(name);
    if (!tool)
        return errorResult(QStringLiteral("未知工具: %1").arg(name));

    if (tool->dangerous) {
        if (!m_writeEnabled)
            return errorResult(QStringLiteral("工具 %1 为写操作，当前 MCP 写入闸门未开启（在 FieldLink 菜单中启动 MCP 服务时选择允许写入）。").arg(name));

        if (m_confirmHandler) {
            const bool approved = m_confirmHandler(name, arguments);
            broadcastLogNotification(QStringLiteral("warning"),
                                     QStringLiteral("危险工具 %1 人工确认结果: %2").arg(name, approved ? QStringLiteral("允许") : QStringLiteral("拒绝")));
            if (!approved)
                return errorResult(QStringLiteral("用户在 FieldLink 确认框中拒绝了此次写操作（工具 %1）。").arg(name));
        }
    }

    return m_tools->callTool(name, arguments, m_writeEnabled);
}

QJsonObject McpServer::handleLoggingSetLevel(const QJsonObject &params)
{
    const QString level = params.value(QStringLiteral("level")).toString().trimmed().toLower();
    m_logLevel = logLevelRank(level.isEmpty() ? QStringLiteral("info") : level);
    emit logLine(QStringLiteral("MCP 日志级别设置为 %1").arg(level));
    return QJsonObject{};
}

// ---------------- resources ----------------

QJsonArray McpServer::resourceDefinitions() const
{
    auto resource = [](const QString &uri, const QString &name, const QString &description) {
        return QJsonObject{
            {QStringLiteral("uri"), uri},
            {QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("mimeType"), QStringLiteral("application/json")}
        };
    };
    return QJsonArray{
        resource(QStringLiteral("fieldlink://status"),
                 QStringLiteral("系统状态"),
                 QStringLiteral("连接状态、轮询状态、报警数量、最近通信时间等运行快照")),
        resource(QStringLiteral("fieldlink://devices"),
                 QStringLiteral("设备列表"),
                 QStringLiteral("全部 Modbus 设备配置（TCP/RTU、串口参数、从站地址）")),
        resource(QStringLiteral("fieldlink://alarms/rules"),
                 QStringLiteral("报警规则"),
                 QStringLiteral("当前配置的全部报警规则")),
        resource(QStringLiteral("fieldlink://alarms/history"),
                 QStringLiteral("报警历史"),
                 QStringLiteral("最近的报警事件与确认状态")),
        resource(QStringLiteral("fieldlink://history/last"),
                 QStringLiteral("最近采集记录"),
                 QStringLiteral("历史数据库中最近 100 条采集记录"))
    };
}

QJsonObject McpServer::handleResourcesList()
{
    return QJsonObject{{QStringLiteral("resources"), resourceDefinitions()}};
}

QJsonObject McpServer::handleResourcesRead(const QJsonValue &id, const QJsonObject &params)
{
    const QString uri = params.value(QStringLiteral("uri")).toString();
    QJsonValue data = m_resourceProvider ? m_resourceProvider(uri) : QJsonValue();
    if (data.isUndefined()) {
        return errorResponse(id, kInvalidParams, QStringLiteral("未知资源: %1").arg(uri));
    }
    QJsonObject contents;
    contents["uri"] = uri;
    contents["mimeType"] = QStringLiteral("application/json");
    contents["text"] = QString::fromUtf8(
        QJsonDocument(data.isObject() ? data.toObject() : QJsonObject{{QStringLiteral("data"), data}})
            .toJson(QJsonDocument::Compact));
    QJsonObject result;
    result["contents"] = QJsonArray{contents};
    return result;
}

// ---------------- prompts ----------------

QJsonArray McpServer::promptDefinitions() const
{
    auto argument = [](const QString &name, const QString &description, bool required) {
        return QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("required"), required}
        };
    };
    return QJsonArray{
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("daily_report")},
            {QStringLiteral("description"), QStringLiteral("生成系统运行日报：读取历史数据与报警统计，输出结构化报告")},
            {QStringLiteral("arguments"), QJsonArray{argument(QStringLiteral("date"), QStringLiteral("报告日期，默认今天"), false)}}
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("troubleshoot")},
            {QStringLiteral("description"), QStringLiteral("通信故障排查：结合系统状态与失败统计给出排查建议")},
            {QStringLiteral("arguments"), QJsonArray{argument(QStringLiteral("symptom"), QStringLiteral("观察到的故障现象"), true)}}
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("alarm_review")},
            {QStringLiteral("description"), QStringLiteral("报警体检：审视现有报警规则是否合理，提出优化建议")},
            {QStringLiteral("arguments"), QJsonArray{}}
        }
    };
}

QJsonObject McpServer::handlePromptsList()
{
    return QJsonObject{{QStringLiteral("prompts"), promptDefinitions()}};
}

QJsonObject McpServer::handlePromptsGet(const QJsonObject &params)
{
    const QString name = params.value(QStringLiteral("name")).toString();
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();

    auto promptResponse = [](const QString &description, const QString &text) {
        QJsonObject message;
        message["role"] = QStringLiteral("user");
        message["content"] = QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), text}};
        QJsonObject result;
        result["description"] = description;
        result["messages"] = QJsonArray{message};
        return result;
    };

    if (name == QStringLiteral("daily_report")) {
        const QString date = arguments.value(QStringLiteral("date")).toString(
            QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
        return promptResponse(QStringLiteral("生成 %1 运行日报").arg(date),
            QStringLiteral(
                "请为 FieldLink 生成 %1 的运行日报。\n"
                "步骤：1) 调用 get_system_status 了解当前运行状态；"
                "2) 调用 query_history 查询当天 00:00 至今的历史数据；"
                "3) 调用 get_alarm_history 查看当天报警。\n"
                "输出：数据采集概况（条数/设备/寄存器分布）、报警摘要、通信质量评估、需要关注的风险点。")
                .arg(date));
    }
    if (name == QStringLiteral("troubleshoot")) {
        const QString symptom = arguments.value(QStringLiteral("symptom")).toString(QStringLiteral("设备通信失败"));
        return promptResponse(QStringLiteral("排查故障"),
            QStringLiteral(
                "FieldLink 出现如下现象：%1。\n"
                "请先调用 get_system_status 与 list_devices 收集现场信息，"
                "再结合连续失败次数与最近通信时间给出可能原因（接线/地址/波特率/超时/网络）与分步排查建议。")
                .arg(symptom));
    }
    if (name == QStringLiteral("alarm_review")) {
        return promptResponse(QStringLiteral("报警规则体检"),
            QStringLiteral(
                "请调用 get_alarm_rules 获取当前全部报警规则，从阈值合理性、条件类型、去抖时间、"
                "报警风暴风险等角度逐条点评，并用 add_alarm_rule 建议补充缺失的规则（先征求确认）。"));
    }
    return errorResponse(QJsonValue(), kInvalidParams, QStringLiteral("未知提示词: %1").arg(name));
}

// ---------------- 通知推送（SSE） ----------------

int McpServer::logLevelRank(const QString &level)
{
    if (level == QLatin1String("debug"))   return 0;
    if (level == QLatin1String("info"))    return 1;
    if (level == QLatin1String("notice"))  return 2;
    if (level == QLatin1String("warning")) return 3;
    return 4;   // error/critical/alert/emergency
}

void McpServer::broadcastNotification(const QJsonObject &notification)
{
    if (m_sseClients.isEmpty())
        return;
    const QByteArray frame = "data: "
        + QJsonDocument(notification).toJson(QJsonDocument::Compact) + "\n\n";
    QList<QTcpSocket *> dead;
    for (QTcpSocket *client : qAsConst(m_sseClients)) {
        if (client->state() == QAbstractSocket::ConnectedState) {
            client->write(frame);
            client->flush();
        } else {
            dead.append(client);
        }
    }
    for (QTcpSocket *client : dead) {
        m_sseClients.removeOne(client);
    }
}

void McpServer::broadcastLogNotification(const QString &level, const QString &data)
{
    if (logLevelRank(level) < m_logLevel)
        return;   // 低于客户端 logging/setLevel 设定的级别则不推送
    broadcastNotification(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("notifications/message")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("level"), level},
            {QStringLiteral("data"), data},
            {QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}}}
    });
}

void McpServer::notifyToolsListChanged()
{
    broadcastNotification(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("notifications/tools/list_changed")}
    });
}

void McpServer::onHeartbeatTick()
{
    if (m_sseClients.isEmpty())
        return;
    // SSE 注释行心跳：保持连接活性并探测死链
    static const QByteArray kKeepalive = ": keepalive\n\n";
    QList<QTcpSocket *> dead;
    for (QTcpSocket *client : qAsConst(m_sseClients)) {
        if (client->state() == QAbstractSocket::ConnectedState) {
            client->write(kKeepalive);
            client->flush();
        } else {
            dead.append(client);
        }
    }
    for (QTcpSocket *client : dead) {
        m_sseClients.removeOne(client);
    }
}
