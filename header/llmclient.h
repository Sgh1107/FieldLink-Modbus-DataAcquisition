#ifndef LLMCLIENT_H
#define LLMCLIENT_H

// llmclient.h
// OpenAI 兼容 chat/completions 客户端（QNetworkAccessManager，非流式）。
//
// 兼容所有 OpenAI 格式服务：DeepSeek / 通义千问 / GLM / Kimi / 本地 Ollama(/v1) 等。
// 支持工具调用（function calling）：传入 MCP 风格工具清单
//   [{name, description, inputSchema}]，自动转换为 OpenAI 的
//   tools:[{type:"function", function:{name, description, parameters}}] 格式。
//
// 应答通过 chatFinished 返回"助手消息"（保持 OpenAI 原始形状，
// tool_calls[].function.arguments 为 JSON 字符串，由调用方解析）。

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class LlmClient : public QObject
{
    Q_OBJECT

public:
    explicit LlmClient(QObject *parent = nullptr);

    // baseUrl 形如 https://api.deepseek.com/v1 或 http://127.0.0.1:11434/v1
    // （内部自动拼接 /chat/completions）；apiKey 为空则不发送 Authorization 头（Ollama）
    void setEndpoint(const QString &baseUrl, const QString &apiKey, const QString &model);
    QString endpointInfo() const;

    // 发起一轮对话请求。messages 为 OpenAI 消息数组；
    // tools 为 MCP 风格工具定义数组（可为空数组 = 不带工具能力）。
    // 结果经 chatFinished / chatFailed 异步返回；同一时刻仅允许一个在途请求。
    void chat(const QJsonArray &messages, const QJsonArray &tools);
    bool isBusy() const;

signals:
    // assistantMessage：OpenAI 形状的助手消息
    //   { "role":"assistant", "content":"...", "tool_calls":[ {id, type:"function",
    //     function:{name, arguments:"{...}"} } ] }（无工具调用时省略 tool_calls）
    void chatFinished(const QJsonObject &assistantMessage);
    void chatFailed(const QString &error);

private slots:
    void onReplyFinished();

private:
    static QJsonArray convertToolsToOpenAI(const QJsonArray &mcpTools);

    QNetworkAccessManager *m_nam;
    QNetworkReply *m_activeReply;
    QString m_baseUrl;
    QString m_apiKey;
    QString m_model;
};

#endif // LLMCLIENT_H
