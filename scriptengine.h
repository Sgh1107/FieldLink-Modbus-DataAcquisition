#ifndef SCRIPTENGINE_H
#define SCRIPTENGINE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QJSEngine>

class ScriptEngine : public QObject
{
    Q_OBJECT

public:
    explicit ScriptEngine(QObject *parent = nullptr);

    bool loadScript(const QString &script);
    bool loadScriptFile(const QString &filePath);
    QVariant execute(const QString &functionName, const QVariantList &args = QVariantList());
    QVariant evaluate(const QString &expression);
    QString lastError() const;

    void setGlobalObject(const QString &name, QObject *object);
    void setGlobalValue(const QString &name, const QVariant &value);

    QStringList availableFunctions() const;

signals:
    void scriptOutput(const QString &message);
    void scriptError(const QString &error);

private:
    QJSEngine m_engine;
    QString m_lastError;
    QStringList m_functions;
};

#endif // SCRIPTENGINE_H
