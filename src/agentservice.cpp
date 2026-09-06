// agentservice.cpp
// Agent ReAct 循环实现。
//
// OpenAI 工具调用约定：
//   助手消息可能携带 tool_calls:[{id, type:"function", function:{name, arguments:"<JSON 字符串>"}}]
//   每个调用必须以 {"role":"tool", "tool_call_id":<id>, "content":<结果文本>} 回填后再请求下一轮。
//   助手消息（含 tool_calls 原始形状）需原样回存进上下文。

#include "agentservice.h"

#include "agenttool.h"
#include "llmclient.h"

#include <QJsonDocument>

namespace {

// 追加一条消息到上下文
void appendMessage(QJsonArray &history, const QString &role, const QString &content)
{
    history.append(QJsonObject{
        {QStringLiteral("role"), role},
        {QStringLiteral("content"), content}});
}

QString compactJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

AgentService::AgentService(QObject *parent)
    : QObject(parent)
    , m_tools(nullptr)
    , m_llm(nullptr)
    , m_maxIterations(8)
    , m_iteration(0)
    , m_busy(false)
{
    setSystemPrompt(QStringLiteral(
        "你是 FieldLink 工业级 Modbus 数据采集平台的内置 AI 助手。"
        "你可以调用工具完成：查询系统状态与设备列表、读取/写入 Modbus 寄存器、查询 SQLite 历史数据、"
        "管理报警规则、控制轮询任务等。写寄存器、修改报警规则、启停轮询属于危险操作，"
        "每次调用都会弹窗由本机操作员确认。回答使用中文，简洁、专业、面向现场运维；"
        "数值解读时注意寄存器为原始值，可能需要按点表换算。"));
}

void AgentService::setToolRegistry(AgentToolRegistry *registry)
{
    m_tools = registry;
}

void AgentService::setLlmClient(LlmClient *client)
{
    if (m_llm)
        disconnect(m_llm, nullptr, this, nullptr);
    m_llm = client;
    if (m_llm) {
        connect(m_llm, &LlmClient::chatFinished, this, &AgentService::onChatFinished);
        connect(m_llm, &LlmClient::chatFailed, this, &AgentService::onChatFailed);
    }
}

void AgentService::setDangerousConfirmHandler(
    const std::function<bool(const QString &toolName, const QJsonObject &arguments)> &handler)
{
    m_confirmHandler = handler;
}

void AgentService::setSystemPrompt(const QString &prompt)
{
    m_systemPrompt = prompt;
}

void AgentService::setMaxToolIterations(int iterations)
{
    m_maxIterations = iterations > 0 ? iterations : 8;
}

bool AgentService::isBusy() const
{
    return m_busy;
}

void AgentService::sendUserMessage(const QString &text)
{
    if (m_busy || text.trimmed().isEmpty())
        return;
    appendMessage(m_history, QStringLiteral("user"), text);
    emit userMessageAdded(text);

    m_busy = true;
    m_iteration = 0;
    emit busyChanged(true);
    requestChat();
}

void AgentService::resetConversation()
{
    if (m_busy) {
        emit errorOccurred(QStringLiteral("AI 正在处理上一条消息，请稍后再清空会话"));
        return;
    }
    m_history = QJsonArray();
}

void AgentService::requestChat()
{
    if (!m_llm || !m_tools) {
        emit errorOccurred(QStringLiteral("Agent 未就绪：缺少 LLM 客户端或工具注册表"));
        finishTurn();
        return;
    }
    // system 消息动态拼在最前，历史数据不含它
    QJsonArray payload;
    payload.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("content"), m_systemPrompt}});
    for (const QJsonValue &message : m_history)
        payload.append(message);

    m_llm->chat(payload, m_tools->toolDefinitions());
}

void AgentService::onChatFinished(const QJsonObject &assistantMessage)
{
    // 原样回存助手消息（保持 tool_calls 的 OpenAI 形状，供下一轮请求使用）
    m_history.append(assistantMessage);

    const QJsonArray toolCalls = assistantMessage.value(QStringLiteral("tool_calls")).toArray();
    const QString content = assistantMessage.value(QStringLiteral("content")).toString();

    if (!toolCalls.isEmpty()) {
        if (m_iteration >= m_maxIterations) {
            // 防失控：超过轮次上限，强制收尾
            appendMessage(m_history, QStringLiteral("assistant"),
                          QStringLiteral("（已达到工具调用轮次上限，停止继续调用）"));
            emit assistantReply(QStringLiteral("工具调用轮次已达上限（%1 轮），已停止。请尝试更明确的问题。")
                                    .arg(m_maxIterations));
            finishTurn();
            return;
        }
        ++m_iteration;
        handleToolCalls(toolCalls);
        requestChat();          // 工具结果已回填，继续下一轮推理
        return;
    }

    if (!content.isEmpty()) {
        emit assistantReply(content);
        finishTurn();
        return;
    }

    emit errorOccurred(QStringLiteral("模型返回了空内容且无工具调用"));
    finishTurn();
}

void AgentService::onChatFailed(const QString &error)
{
    emit errorOccurred(error);
    finishTurn();
}

void AgentService::handleToolCalls(const QJsonArray &toolCalls)
{
    for (const QJsonValue &call : toolCalls) {
        const QJsonObject callObject = call.toObject();
        const QString callId = callObject.value(QStringLiteral("id")).toString();
        const QJsonObject function = callObject.value(QStringLiteral("function")).toObject();
        const QString name = function.value(QStringLiteral("name")).toString();

        // arguments 是 JSON 字符串，解析为对象；失败按空参数处理
        QJsonObject arguments;
        const QString rawArguments = function.value(QStringLiteral("arguments")).toString();
        const QJsonDocument argsDoc = QJsonDocument::fromJson(rawArguments.toUtf8());
        if (argsDoc.isObject())
            arguments = argsDoc.object();

        m_history.append(executeOneTool(callId, name, arguments));
    }
}

QJsonObject AgentService::executeOneTool(const QString &callId, const QString &name,
                                         const QJsonObject &arguments)
{
    emit toolStarted(name, compactJson(arguments));

    auto toolMessage = [&](bool ok, const QString &resultText) {
        emit toolFinished(name, ok, resultText.left(200));
        return QJsonObject{
            {QStringLiteral("role"), QStringLiteral("tool")},
            {QStringLiteral("tool_call_id"), callId},
            {QStringLiteral("content"), resultText}};
    };

    if (!m_tools)
        return toolMessage(false, QStringLiteral("{\"error\":\"tool registry not ready\"}"));

    const AgentTool *tool = m_tools->tool(name);
    if (!tool)
        return toolMessage(false, QStringLiteral("{\"error\":\"未知工具: %1\"}").arg(name));

    // 危险工具：与 MCP 相同的人工确认闸门（写操作必须本机操作员逐次批准）
    if (tool->dangerous && m_confirmHandler) {
        if (!m_confirmHandler(name, arguments)) {
            return toolMessage(false,
                QStringLiteral("{\"error\":\"用户在确认框中拒绝了此次写操作\"}"));
        }
    }

    // 注册表内部会做参数 Schema 校验；writeAllowed=true：Agent 的写闸门由确认框承担
    const QJsonObject result = m_tools->callTool(name, arguments, true);
    const bool isError = result.value(QStringLiteral("isError")).toBool(false);
    const QString resultText = result.value(QStringLiteral("content")).toArray()
                                   .first().toObject()
                                   .value(QStringLiteral("text")).toString();
    return toolMessage(!isError, resultText);
}

void AgentService::finishTurn()
{
    m_busy = false;
    emit busyChanged(false);
}
