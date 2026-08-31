#ifndef AGENTTOOL_H
#define AGENTTOOL_H

// agenttool.h
// AI Agent / MCP 共用的"工具注册表"。
// 设计文档：doc/AI_AGENT_MCP_DESIGN.md —— 一份工具定义，两个出口：
//   1) McpServer  : MCP 客户端（Claude Desktop / Cursor 等）经 MCP 协议调用
//   2) agent 分支 : 内嵌 AI 助手经 LLM Function Calling 调用
// 每个工具 = 名称 + 描述 + JSON Schema 参数定义 + 处理函数。

#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

struct AgentTool {
    QString name;              // 工具名（MCP / function calling 通用，蛇形命名）
    QString description;       // 给 LLM 看的说明：什么时候该用、返回什么
    QJsonObject inputSchema;   // 参数 JSON Schema：{ type:"object", properties:{...}, required:[...] }
    bool dangerous = false;    // true = 写操作，受写入闸门（writeEnabled）控制
    std::function<QJsonObject(const QJsonObject &arguments)> handler;

    // ---- Schema 构造辅助（减少手写 JSON 的重复劳动）----

    // 单个属性定义：{"type": type, "description": desc}
    static QJsonObject property(const QString &type, const QString &description)
    {
        return QJsonObject{{"type", type}, {"description", description}};
    }

    // 整个 object 级 Schema：{"type":"object", "description":..., "properties":{...}, "required":[...]}
    static QJsonObject makeSchema(const QString &description,
                                  const QJsonObject &properties,
                                  const QStringList &required = QStringList())
    {
        QJsonObject schema;
        schema["type"] = QStringLiteral("object");
        schema["description"] = description;
        schema["properties"] = properties;
        if (!required.isEmpty()) {
            QJsonArray req;
            for (const QString &r : required)
                req.append(r);
            schema["required"] = req;
        }
        return schema;
    }
};

class AgentToolRegistry
{
public:
    void registerTool(const AgentTool &tool);

    bool contains(const QString &name) const;
    const AgentTool *tool(const QString &name) const;
    QVector<AgentTool> tools() const;
    int count() const;

    // MCP tools/list 的 tools 数组
    QJsonArray toolDefinitions() const;

    // 依据 inputSchema 校验参数（required 缺失 / 类型不符），返回错误描述，通过则返回空串
    QString validateArguments(const AgentTool &tool, const QJsonObject &arguments) const;

    // 执行工具，返回 MCP tools/call 的 result：
    //   { content:[{type:"text", text:...}], structuredContent:{...}, isError?:true }
    // writeAllowed = false 时，dangerous 工具返回 isError 结果（不执行）
    QJsonObject callTool(const QString &name, const QJsonObject &arguments, bool writeAllowed) const;

private:
    static QString jsonTypeOf(const QJsonValue &value);

    QVector<AgentTool> m_tools;
};

#endif // AGENTTOOL_H
