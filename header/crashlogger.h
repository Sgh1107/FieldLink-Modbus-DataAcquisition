#ifndef CRASHLOGGER_H
#define CRASHLOGGER_H

#include <QObject>
#include <QString>

class CrashLogger : public QObject
{
    Q_OBJECT

public:
    explicit CrashLogger(QObject *parent = nullptr);
    static void install();
    static QString crashLogDirectory();
    static void writeRuntimeMarker(const QString &message);
};

#endif // CRASHLOGGER_H
