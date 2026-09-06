// agentchatpanel.cpp
// AI 助手聊天窗口实现。会话与配置：
//   - 配置持久化在 QSettings "ai/" 配置节（baseUrl/apiKey/model）
//   - 默认指向 DeepSeek OpenAI 兼容端点；本地 Ollama 填 http://127.0.0.1:11434/v1，密钥留空

#include "agentchatpanel.h"

#include "agentservice.h"
#include "llmclient.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
const QString kDefaultBaseUrl = QStringLiteral("https://api.deepseek.com/v1");
const QString kDefaultModel = QStringLiteral("deepseek-chat");
}

AgentChatPanel::AgentChatPanel(AgentService *service, LlmClient *llmClient, QWidget *parent)
    : QDialog(parent)
    , m_service(service)
    , m_llm(llmClient)
    , m_chatView(new QTextBrowser(this))
    , m_input(new QLineEdit(this))
    , m_sendButton(new QPushButton(QStringLiteral("发送"), this))
    , m_urlEdit(new QLineEdit(this))
    , m_keyEdit(new QLineEdit(this))
    , m_modelEdit(new QLineEdit(this))
    , m_statusLabel(new QLabel(this))
{
    setWindowTitle(QStringLiteral("AI 助手（FieldLink）"));
    resize(640, 620);

    auto *rootLayout = new QVBoxLayout(this);

    // ---- LLM 服务配置区 ----
    auto *configForm = new QFormLayout;
    m_urlEdit->setPlaceholderText(QStringLiteral("https://api.deepseek.com/v1 或 http://127.0.0.1:11434/v1"));
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(QStringLiteral("本地 Ollama 可留空"));
    m_modelEdit->setPlaceholderText(QStringLiteral("deepseek-chat / qwen-plus / llama3 ..."));
    configForm->addRow(QStringLiteral("服务地址"), m_urlEdit);
    configForm->addRow(QStringLiteral("API Key"), m_keyEdit);
    configForm->addRow(QStringLiteral("模型"), m_modelEdit);
    auto *saveConfigButton = new QPushButton(QStringLiteral("保存配置"), this);
    configForm->addRow(QString(), saveConfigButton);
    rootLayout->addLayout(configForm);

    // ---- 对话视图 ----
    m_chatView->setOpenExternalLinks(false);
    rootLayout->addWidget(m_chatView, 1);

    // ---- 输入行 ----
    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_sendButton);
    rootLayout->addLayout(inputRow);

    // ---- 底部状态行 ----
    auto *bottomRow = new QHBoxLayout;
    m_statusLabel->setText(QStringLiteral("就绪"));
    auto *clearButton = new QPushButton(QStringLiteral("清空会话"), this);
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(clearButton);
    rootLayout->addLayout(bottomRow);

    loadConfig();

    connect(saveConfigButton, &QPushButton::clicked, this, &AgentChatPanel::onSaveConfig);
    connect(clearButton, &QPushButton::clicked, this, &AgentChatPanel::onClearConversation);
    connect(m_sendButton, &QPushButton::clicked, this, &AgentChatPanel::onSend);
    connect(m_input, &QLineEdit::returnPressed, this, &AgentChatPanel::onSend);

    connect(m_service, &AgentService::userMessageAdded, this, [this](const QString &text) {
        appendLine(QStringLiteral("<p><b style=\"color:#2980b9\">你：</b>%1</p>").arg(text.toHtmlEscaped()));
    });
    connect(m_service, &AgentService::assistantReply, this, &AgentChatPanel::onAssistantReply);
    connect(m_service, &AgentService::toolStarted, this, &AgentChatPanel::onToolStarted);
    connect(m_service, &AgentService::toolFinished, this, &AgentChatPanel::onToolFinished);
    connect(m_service, &AgentService::errorOccurred, this, &AgentChatPanel::onError);
    connect(m_service, &AgentService::busyChanged, this, &AgentChatPanel::onBusyChanged);

    appendLine(QStringLiteral(
        "<p style=\"color:#7f8c8d\">AI 助手已就绪。可询问系统状态、读取寄存器、查询历史数据；"
        "写寄存器/改报警规则等危险操作会弹窗请你确认。</p>"));
}

void AgentChatPanel::loadConfig()
{
    QSettings settings;
    m_urlEdit->setText(settings.value(QStringLiteral("ai/baseUrl"), kDefaultBaseUrl).toString());
    m_keyEdit->setText(settings.value(QStringLiteral("ai/apiKey")).toString());
    m_modelEdit->setText(settings.value(QStringLiteral("ai/model"), kDefaultModel).toString());

    m_llm->setEndpoint(m_urlEdit->text().trimmed(), m_keyEdit->text().trimmed(),
                       m_modelEdit->text().trimmed());
}

void AgentChatPanel::onSaveConfig()
{
    QSettings settings;
    settings.setValue(QStringLiteral("ai/baseUrl"), m_urlEdit->text().trimmed());
    settings.setValue(QStringLiteral("ai/apiKey"), m_keyEdit->text().trimmed());
    settings.setValue(QStringLiteral("ai/model"), m_modelEdit->text().trimmed());
    m_llm->setEndpoint(m_urlEdit->text().trimmed(), m_keyEdit->text().trimmed(),
                       m_modelEdit->text().trimmed());
    m_statusLabel->setText(QStringLiteral("配置已保存"));
    appendLine(QStringLiteral("<p style=\"color:#7f8c8d\">LLM 配置已更新：%1</p>")
                   .arg(m_llm->endpointInfo().toHtmlEscaped()));
}

void AgentChatPanel::onSend()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty())
        return;
    m_input->clear();
    m_service->sendUserMessage(text);
}

void AgentChatPanel::onAssistantReply(const QString &text)
{
    appendLine(QStringLiteral("<p><b style=\"color:#27ae60\">AI：</b>%1</p>").arg(text.toHtmlEscaped()));
}

void AgentChatPanel::onToolStarted(const QString &name, const QString &argsSummary)
{
    appendLine(QStringLiteral("<p style=\"color:#8e44ad;margin-left:16px\">⚙ 调用工具 <b>%1</b> 参数 %2 ...</p>")
                   .arg(name.toHtmlEscaped(), argsSummary.toHtmlEscaped()));
}

void AgentChatPanel::onToolFinished(const QString &name, bool ok, const QString &resultSummary)
{
    appendLine(QStringLiteral("<p style=\"color:%1;margin-left:16px\">　↳ %2 %3</p>")
                   .arg(ok ? QStringLiteral("#27ae60") : QStringLiteral("#c0392b"),
                        ok ? QStringLiteral("完成") : QStringLiteral("失败"),
                        resultSummary.toHtmlEscaped()));
}

void AgentChatPanel::onError(const QString &message)
{
    appendLine(QStringLiteral("<p style=\"color:#c0392b\"><b>错误：</b>%1</p>").arg(message.toHtmlEscaped()));
}

void AgentChatPanel::onBusyChanged(bool busy)
{
    m_sendButton->setEnabled(!busy);
    m_input->setEnabled(!busy);
    m_statusLabel->setText(busy ? QStringLiteral("AI 正在思考/执行工具...") : QStringLiteral("就绪"));
}

void AgentChatPanel::onClearConversation()
{
    m_service->resetConversation();
    m_chatView->clear();
    appendLine(QStringLiteral("<p style=\"color:#7f8c8d\">会话已清空。</p>"));
}

void AgentChatPanel::appendLine(const QString &html)
{
    m_chatView->append(html);
}
