#ifndef MCPSERVER_H
#define MCPSERVER_H

// mcpserver.h
// FieldLink 内置 MCP (Model Context Protocol) 服务器。
//
// 借鉴用户已有项目 pros/mcp_server 的设计（会话管理 / SSE 推送 / 通知体系），
// 结合工业场景权限边界要求（写入闸门 / GUI 人工确认 / 限流 / 鉴权 / 重绑定防护）。
//
// 传输层  : Streamable HTTP（MCP 2025-06-18 规范）
//           - POST /mcp   : JSON-RPC 2.0 请求，application/json 应答
//           - GET  /mcp   : SSE 长连接，接收服务端主动通知（notifications/message 等）
//           - DELETE /mcp : 终止会话
//           - initialize 时签发 Mcp-Session-Id；默认宽容模式（未携带也能工作，兼容
//             无会话意识的简单客户端），可切换为强制会话校验
// 能力层  : tools（AgentToolRegistry）/ resources / prompts + logging/setLevel
// 安全层  : Token 鉴权、写入闸门、危险工具 GUI 人工确认、IP 滑动窗口限流、
//           Origin 校验（DNS 重绑定防护）、全量审计日志

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QHash>
#include <QDateTime>
#include <QString>
#include <functional>

#include "agenttool.h"

class McpServer : public QObject
{
    Q_OBJECT

public:
    explicit McpServer(QObject *parent = nullptr);
    ~McpServer();

    bool start(quint16 port = 8180);
    void stop();
    bool isRunning() const;
    quint16 port() const;

    void setToolRegistry(AgentToolRegistry *registry);   // 不取得所有权
    void setAuthToken(const QString &token);             // 空 = 不校验（仅建议本机调试时使用）
    void setWriteEnabled(bool enabled);                  // dangerous 工具的写入闸门
    bool writeEnabled() const;

    // 强制会话模式：true 时未携带有效 Mcp-Session-Id 的请求返回 404。
    // 默认 false（宽容模式）：签发会话但不强制，兼容 stdio 桥等简单客户端。
    void setRequireSession(bool enabled);
    bool requireSession() const;

    // 危险工具人工确认（human-in-the-loop）：
    // 写入闸门开启时，dangerous 工具调用前回调；返回 false 则拒绝执行。
    // 由 MainWindow 提供实现（弹出 GUI 确认框），在主线程同步执行。
    void setDangerousConfirmHandler(
        const std::function<bool(const QString &toolName, const QJsonObject &arguments)> &handler);

    // 每个 IP 每分钟允许的 POST 请求数（滑动窗口），超出返回 429；<=0 表示不限
    void setRateLimitPerMinute(int limit);
    int rateLimitPerMinute() const;

    // 资源数据提供者：uri -> JSON 值；未知 uri 返回 QJsonValue()（undefined）
    void setResourceProvider(const std::function<QJsonValue(const QString &uri)> &provider);

    // 广播 notifications/tools/list_changed（工具集动态变化时由外部触发）
    void notifyToolsListChanged();

signals:
    void runningChanged(bool running);
    void logLine(const QString &message);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onClientDisconnected();
    void onHeartbeatTick();     // SSE 保活
    void onSessionSweep();      // 过期会话清理

private:
    // ---- 会话 ----
    struct McpSession {
        QDateTime lastActive;
    };
    QString ensureSession();                        // 新建会话并返回 id
    bool touchSession(const QString &sessionId);    // 存在则刷新活跃时间
    void endSession(const QString &sessionId);

    // ---- HTTP 层 ----
    void processRequest(QTcpSocket *socket, const QByteArray &data);
    void parseHttpRequest(const QByteArray &data, QString &method, QString &path,
                          QMap<QString, QString> &headers, QByteArray &body) const;
    bool authorized(const QMap<QString, QString> &headers) const;
    bool isRateLimited(const QString &clientKey);
    void sendHttpResponse(QTcpSocket *socket, int statusCode, const QByteArray &body,
                          const QByteArray &extraHeaders = QByteArray());
    void startSseStream(QTcpSocket *socket, const QMap<QString, QString> &headers);

    // ---- JSON-RPC / MCP 协议层 ----
    QByteArray handleRpcPayload(const QJsonDocument &doc);          // 兼容单请求与数组
    QByteArray dispatch(const QJsonObject &request);                // 单条请求 -> 响应 JSON（通知返回空）
    QJsonObject resultResponse(const QJsonValue &id, const QJsonObject &result) const;
    QJsonObject errorResponse(const QJsonValue &id, int code, const QString &message) const;

    QJsonObject handleInitialize(const QJsonObject &request);
    QJsonObject handleToolsList();
    QJsonObject handleToolsCall(const QJsonObject &params);
    QJsonObject handleResourcesList();
    QJsonObject handleResourcesRead(const QJsonValue &id, const QJsonObject &params);
    QJsonObject handlePromptsList();
    QJsonObject handlePromptsGet(const QJsonObject &params);
    QJsonObject handleLoggingSetLevel(const QJsonObject &params);
    QJsonArray resourceDefinitions() const;
    QJsonArray promptDefinitions() const;

    // ---- 通知推送（SSE） ----
    void broadcastNotification(const QJsonObject &notification);
    void broadcastLogNotification(const QString &level, const QString &data);
    static int logLevelRank(const QString &level);                  // debug<info<notice<warning<error

    QTcpServer *m_server;
    QList<QTcpSocket *> m_clients;
    QMap<QTcpSocket *, QByteArray> m_requestBuffers;   // 每连接请求缓冲（Content-Length 对齐）
    QList<QTcpSocket *> m_sseClients;                  // GET /mcp 建立的通知订阅连接
    quint16 m_port;
    QString m_authToken;
    bool m_writeEnabled;
    bool m_requireSession;
    int m_logLevel;                                    // 通知推送的最低日志级别
    int m_rateLimitPerMin;
    AgentToolRegistry *m_tools;
    std::function<QJsonValue(const QString &uri)> m_resourceProvider;
    std::function<bool(const QString &toolName, const QJsonObject &arguments)> m_confirmHandler;

    // 会话表（id -> 活跃时间），initialize 时签发
    QHash<QString, McpSession> m_sessions;
    QString m_issuedSessionId;                         // 本次 initialize 响应要附带的会话头

    // 限流：clientKey -> 最近请求时间戳（滑动窗口）
    QHash<QString, QVector<qint64>> m_rateWindows;

    QTimer m_heartbeatTimer;                           // SSE 心跳（15s）
    QTimer m_sessionSweepTimer;                        // 过期会话清理（60s）
};

#endif // MCPSERVER_H
