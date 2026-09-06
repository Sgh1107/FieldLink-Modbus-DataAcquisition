// llmclient.cpp
// OpenAI 兼容 chat/completions 客户端实现（非流式）。
//
// 请求体：{ model, messages, tools? , tool_choice:"auto" }
// 应答体：{ choices:[{ message:{ role, content, tool_calls? }, finish_reason }] }
// 错误体：{ error:{ message } }

#include "llmclient.h"

#include <QJsonDocument>
#include <QNetworkRequest>

namespace {
constexpr int kTransferTimeoutMs = 120 * 1000;   // LLM 推理可能较慢
}

LlmClient::LlmClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_activeReply(nullptr)
{
}

void LlmClient::setEndpoint(const QString &baseUrl, const QString &apiKey, const QString &model)
{
    m_baseUrl = baseUrl;
    // 容错：去掉末尾斜杠，统一拼接
    m_baseUrl = m_baseUrl.trimmed();
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);
    m_apiKey = apiKey.trimmed();
    m_model = model.trimmed();
}

QString LlmClient::endpointInfo() const
{
    return QStringLiteral("%1 [%2]").arg(m_baseUrl, m_model.isEmpty() ? QStringLiteral("--") : m_model);
}

QJsonArray LlmClient::convertToolsToOpenAI(const QJsonArray &mcpTools)
{
    // MCP: {name, description, inputSchema} → OpenAI: {type:"function", function:{name, description, parameters}}
    QJsonArray out;
    for (const QJsonValue &value : mcpTools) {
        const QJsonObject tool = value.toObject();
        QJsonObject function;
        function[QStringLiteral("name")] = tool.value(QStringLiteral("name"));
        function[QStringLiteral("description")] = tool.value(QStringLiteral("description"));
        function[QStringLiteral("parameters")] = tool.value(QStringLiteral("inputSchema"));
        out.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), function}});
    }
    return out;
}

void LlmClient::chat(const QJsonArray &messages, const QJsonArray &tools)
{
    if (m_activeReply) {
        emit chatFailed(QStringLiteral("上一次 LLM 请求尚未完成"));
        return;
    }
    if (m_baseUrl.isEmpty() || m_model.isEmpty()) {
        emit chatFailed(QStringLiteral("LLM 服务地址或模型未配置"));
        return;
    }

    QJsonObject body;
    body[QStringLiteral("model")] = m_model;
    body[QStringLiteral("messages")] = messages;
    body[QStringLiteral("tool_choice")] = QStringLiteral("auto");
    if (!tools.isEmpty())
        body[QStringLiteral("tools")] = convertToolsToOpenAI(tools);

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_apiKey.isEmpty())
        request.setRawHeader(QByteArray("Authorization"), "Bearer " + m_apiKey.toUtf8());
    request.setTransferTimeout(kTransferTimeoutMs);

    m_activeReply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_activeReply, &QNetworkReply::finished, this, &LlmClient::onReplyFinished);
}

bool LlmClient::isBusy() const
{
    return m_activeReply != nullptr;
}

void LlmClient::onReplyFinished()
{
    QNetworkReply *reply = m_activeReply;
    if (!reply)
        return;
    m_activeReply = nullptr;
    reply->deleteLater();

    // 网络层错误
    if (reply->error() != QNetworkReply::NoError) {
        // 4xx/5xx 时 HTTP 错误也带响应体，先尝试读出 error.message
        const QByteArray body = reply->readAll();
        QString detail = QStringLiteral("HTTP %1 %2").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                             .arg(reply->errorString());
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            const QString serverMessage = doc.object().value(QStringLiteral("error"))
                                              .toObject().value(QStringLiteral("message")).toString();
            if (!serverMessage.isEmpty())
                detail = serverMessage;
        }
        emit chatFailed(QStringLiteral("LLM 请求失败: %1").arg(detail));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        emit chatFailed(QStringLiteral("LLM 应答不是有效 JSON"));
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        emit chatFailed(QStringLiteral("LLM 应答缺少 choices"));
        return;
    }
    const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();

    // 规范化：content 永远是字符串（部分服务对纯工具调用返回 null）
    QJsonObject normalized = message;
    if (normalized.value(QStringLiteral("content")).type() != QJsonValue::String)
        normalized[QStringLiteral("content")] = QString();
    emit chatFinished(normalized);
}
