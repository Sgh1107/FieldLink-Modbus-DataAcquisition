#ifndef AGENTCHATPANEL_H
#define AGENTCHATPANEL_H

// agentchatpanel.h
// 内嵌 AI 助手的聊天窗口（非模态对话框，会话跨开关保留）。
//
// 功能：LLM 服务配置（OpenAI 兼容地址/密钥/模型）、对话视图（用户/AI/工具调用分色）、
//       输入发送、清空会话；危险写操作确认由 MainWindow 的确认框承担。

#include <QDialog>

class QTextBrowser;
class QLineEdit;
class QPushButton;
class QLabel;
class AgentService;
class LlmClient;

class AgentChatPanel : public QDialog
{
    Q_OBJECT

public:
    explicit AgentChatPanel(AgentService *service, LlmClient *llmClient, QWidget *parent = nullptr);

private slots:
    void onSend();
    void onAssistantReply(const QString &text);
    void onToolStarted(const QString &name, const QString &argsSummary);
    void onToolFinished(const QString &name, bool ok, const QString &resultSummary);
    void onError(const QString &message);
    void onBusyChanged(bool busy);
    void onSaveConfig();
    void onClearConversation();

private:
    void appendLine(const QString &html);
    void loadConfig();

    AgentService *m_service;      // 不取得所有权
    LlmClient *m_llm;             // 不取得所有权
    QTextBrowser *m_chatView;
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    QLineEdit *m_urlEdit;
    QLineEdit *m_keyEdit;
    QLineEdit *m_modelEdit;
    QLabel *m_statusLabel;
};

#endif // AGENTCHATPANEL_H
