// agenttool.cpp
// AgentToolRegistry 实现：工具注册、Schema 校验、统一执行入口。

#include "agenttool.h"

#include <QJsonDocument>

void AgentToolRegistry::registerTool(const AgentTool &tool)
{
    for (const AgentTool &existing : m_tools) {
        if (existing.name == tool.name)
            return; // 同名工具只注册一次
    }
    m_tools.append(tool);
}

bool AgentToolRegistry::contains(const QString &name) const
{
    for (const AgentTool &tool : m_tools) {
        if (tool.name == name)
            return true;
    }
    return false;
}

const AgentTool *AgentToolRegistry::tool(const QString &name) const
{
    for (const AgentTool &tool : m_tools) {
        if (tool.name == name)
            return &tool;
    }
    return nullptr;
}

QVector<AgentTool> AgentToolRegistry::tools() const
{
    return m_tools;
}

int AgentToolRegistry::count() const
{
    return m_tools.count();
}

QJsonArray AgentToolRegistry::toolDefinitions() const
{
    QJsonArray defs;
    for (const AgentTool &tool : m_tools) {
        QJsonObject def;
        def["name"] = tool.name;
        def["description"] = tool.description;
        def["inputSchema"] = tool.inputSchema;
        defs.append(def);
    }
    return defs;
}

QString AgentToolRegistry::jsonTypeOf(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::String:  return QStringLiteral("string");
    case QJsonValue::Double:  return QStringLiteral("number");
    case QJsonValue::Bool:    return QStringLiteral("boolean");
    case QJsonValue::Array:   return QStringLiteral("array");
    case QJsonValue::Object:  return QStringLiteral("object");
    default:                  return QStringLiteral("null");
    }
}

QString AgentToolRegistry::validateArguments(const AgentTool &tool, const QJsonObject &arguments) const
{
    const QJsonObject schema = tool.inputSchema;
    if (schema.isEmpty())
        return QString();

    // required 检查
    const QJsonArray required = schema.value(QStringLiteral("required")).toArray();
    for (const QJsonValue &r : required) {
        const QString key = r.toString();
        if (!arguments.contains(key) || arguments.value(key).isNull())
            return QStringLiteral("缺少必填参数: %1").arg(key);
    }

    // properties 类型检查（number 同时接受整数）
    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    for (QJsonObject::const_iterator it = properties.begin(); it != properties.end(); ++it) {
        if (!arguments.contains(it.key()))
            continue;
        const QString expected = it.value().toObject().value(QStringLiteral("type")).toString();
        const QString actual = jsonTypeOf(arguments.value(it.key()));
        if (actual == QStringLiteral("null"))
            continue;
        if (expected == actual)
            continue;
        if (expected == QStringLiteral("number") && actual == QStringLiteral("number"))
            continue;
        // 整数以 Double 表示，number 接受之；其余类型不符
        if (expected == QStringLiteral("number"))
            continue;
        return QStringLiteral("参数 %1 类型应为 %2，实际为 %3").arg(it.key(), expected, actual);
    }
    return QString();
}

QJsonObject AgentToolRegistry::callTool(const QString &name, const QJsonObject &arguments, bool writeAllowed) const
{
    auto errorResult = [](const QString &message) {
        return QJsonObject{
            {QStringLiteral("content"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), message}}}},
            {QStringLiteral("isError"), true}
        };
    };

    const AgentTool *target = tool(name);
    if (!target)
        return errorResult(QStringLiteral("未知工具: %1").arg(name));

    const QString validationError = validateArguments(*target, arguments);
    if (!validationError.isEmpty())
        return errorResult(QStringLiteral("参数校验失败: %1").arg(validationError));

    if (target->dangerous && !writeAllowed)
        return errorResult(QStringLiteral("工具 %1 为写操作，当前 MCP 写入闸门未开启（在 FieldLink 菜单中启动 MCP 服务时选择允许写入）。").arg(name));

    // 执行处理器
    QJsonObject result = target->handler(arguments);

    // 约定：处理器结果含 "success": false 时视为工具执行失败
    const bool hasSuccessFlag = result.contains(QStringLiteral("success"));
    const bool success = hasSuccessFlag ? result.value(QStringLiteral("success")).toBool(true) : true;

    QJsonObject response;
    QJsonArray content;
    QJsonObject textPart;
    textPart["type"] = QStringLiteral("text");
    textPart["text"] = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    content.append(textPart);
    response["content"] = content;
    response["structuredContent"] = result;
    if (!success)
        response["isError"] = true;
    return response;
}
