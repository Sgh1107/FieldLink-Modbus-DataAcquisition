#ifndef AGENTSERVICE_H
#define AGENTSERVICE_H

// agentservice.h
// 内嵌 AI Agent 的编排核心（ReAct 循环）。
//
// 流程：sendUserMessage → LlmClient::chat → 助手消息
//   ├── 含 tool_calls → 逐个执行（AgentToolRegistry，危险工具先过人工确认）
//   │                   → 以 role:"tool" 回填结果 → 继续请求 LLM（最多 maxIterations 轮）
//   └── 纯文本回答   → assistantReply 信号，一轮对话结束
//
// 与 MCP 路线共用 AgentToolRegistry：同一份工具定义，两个出口（设计文档核心决策）。

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>

class LlmClient;
class AgentToolRegistry;

class AgentService : public QObject
{
    Q_OBJECT

public:
    explicit AgentService(QObject *parent = nullptr);

    void setToolRegistry(AgentToolRegistry *registry);   // 不取得所有权
    void setLlmClient(LlmClient *client);                // 不取得所有权

    // 危险工具（dangerous=true）调用前回调；返回 false = 操作员拒绝。
    // 由 MainWindow 提供（弹出确认框），与 MCP 的确认闸门同一策略。
    void setDangerousConfirmHandler(
        const std::function<bool(const QString &toolName, const QJsonObject &arguments)> &handler);

    void setSystemPrompt(const QString &prompt);
    void setMaxToolIterations(int iterations);           // 防失控上限，默认 8

    bool isBusy() const;

public slots:
    void sendUserMessage(const QString &text);
    void resetConversation();                            // 清空上下文（不影响工具/LLM 配置）

signals:
    void userMessageAdded(const QString &text);          // 回显用户输入
    void assistantReply(const QString &text);            // 本轮最终回答
    void toolStarted(const QString &name, const QString &argsSummary);
    void toolFinished(const QString &name, bool ok, const QString &resultSummary);
    void errorOccurred(const QString &message);
    void busyChanged(bool busy);

private slots:
    void onChatFinished(const QJsonObject &assistantMessage);
    void onChatFailed(const QString &error);

private:
    void requestChat();
    void finishTurn();
    void handleToolCalls(const QJsonArray &toolCalls);
    QJsonObject executeOneTool(const QString &callId, const QString &name,
                               const QJsonObject &arguments);   // 返回 role:"tool" 消息

    QJsonArray m_history;              // 不含 system 消息（请求时动态拼在最前）
    AgentToolRegistry *m_tools;
    LlmClient *m_llm;
    std::function<bool(const QString &, const QJsonObject &)> m_confirmHandler;
    QString m_systemPrompt;
    int m_maxIterations;
    int m_iteration;
    bool m_busy;
};

#endif // AGENTSERVICE_H
