#include "scriptengine.h"
#include <QFile>
#include <QTextStream>
#include <QJSValue>
#include <QRegularExpression>

ScriptEngine::ScriptEngine(QObject *parent)
    : QObject(parent)
{
}

bool ScriptEngine::loadScript(const QString &script)
{
    QJSValue result = m_engine.evaluate(script);
    if (result.isError()) {
        m_lastError = result.toString();
        emit scriptError(m_lastError);
        return false;
    }

    QRegularExpression funcRegex("function\\s+(\\w+)\\s*\\(");
    QRegularExpressionMatchIterator it = funcRegex.globalMatch(script);
    m_functions.clear();
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        m_functions.append(match.captured(1));
    }

    return true;
}

bool ScriptEngine::loadScriptFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = "Cannot open file: " + filePath;
        emit scriptError(m_lastError);
        return false;
    }

    QTextStream in(&file);
    QString script = in.readAll();
    file.close();

    return loadScript(script);
}

QVariant ScriptEngine::execute(const QString &functionName, const QVariantList &args)
{
    QJSValue func = m_engine.globalObject().property(functionName);
    if (!func.isCallable()) {
        m_lastError = "Function not found: " + functionName;
        emit scriptError(m_lastError);
        return QVariant();
    }

    QJSValueList jsArgs;
    for (const QVariant &arg : args) {
        jsArgs.append(m_engine.toScriptValue(arg));
    }

    QJSValue result = func.call(jsArgs);
    if (result.isError()) {
        m_lastError = result.toString();
        emit scriptError(m_lastError);
        return QVariant();
    }

    return result.toVariant();
}

QVariant ScriptEngine::evaluate(const QString &expression)
{
    QJSValue result = m_engine.evaluate(expression);
    if (result.isError()) {
        m_lastError = result.toString();
        emit scriptError(m_lastError);
        return QVariant();
    }
    return result.toVariant();
}

QString ScriptEngine::lastError() const
{
    return m_lastError;
}

void ScriptEngine::setGlobalObject(const QString &name, QObject *object)
{
    QJSValue jsObj = m_engine.newQObject(object);
    m_engine.globalObject().setProperty(name, jsObj);
}

void ScriptEngine::setGlobalValue(const QString &name, const QVariant &value)
{
    m_engine.globalObject().setProperty(name, m_engine.toScriptValue(value));
}

QStringList ScriptEngine::availableFunctions() const
{
    return m_functions;
}
