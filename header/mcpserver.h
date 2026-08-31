#ifndef MCPSERVER_H
#define MCPSERVER_H

// mcpserver.h
// FieldLink 内置 MCP (Model Context Protocol) 服务器。
//
// 传输层  : Streamable HTTP（MCP 2025-06-18 规范）—— JSON-RPC 2.0，
//           客户端 POST /mcp，服务器以 application/json 应答（无状态模式，不分配会话）。
//           逐条 JSON-RPC 处理，同时兼容请求数组（旧版规范的批量语义）。
// 能力层  : tools（来自 AgentToolRegistry）/ resources（系统数据快照）/ prompts（预置提示词）。
// 安全    : 可选 Token 鉴权（Authorization: Bearer / X-Api-Token）+ 独立写入闸门。

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QList>
#include <QMap>
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

    // 资源数据提供者：uri -> JSON 值；未知 uri 返回 QJsonValue()（undefined）
    void setResourceProvider(const std::function<QJsonValue(const QString &uri)> &provider);

signals:
    void runningChanged(bool running);
    void logLine(const QString &message);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onClientDisconnected();

private:
    // ---- HTTP 层 ----
    void processRequest(QTcpSocket *socket, const QByteArray &data);
    void parseHttpRequest(const QByteArray &data, QString &method, QString &path,
                          QMap<QString, QString> &headers, QByteArray &body) const;
    bool authorized(const QMap<QString, QString> &headers) const;
    void sendHttpResponse(QTcpSocket *socket, int statusCode, const QByteArray &body);

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
    QJsonArray resourceDefinitions() const;
    QJsonArray promptDefinitions() const;

    QTcpServer *m_server;
    QList<QTcpSocket *> m_clients;
    QMap<QTcpSocket *, QByteArray> m_requestBuffers;   // 每连接请求缓冲（Content-Length 对齐）
    quint16 m_port;
    QString m_authToken;
    bool m_writeEnabled;
    AgentToolRegistry *m_tools;
    std::function<QJsonValue(const QString &uri)> m_resourceProvider;
};

#endif // MCPSERVER_H
