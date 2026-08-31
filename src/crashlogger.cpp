#include "crashlogger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <csignal>
#include <cstdlib>

static void writeCrashSignal(int signal)
{
    QDir().mkpath(CrashLogger::crashLogDirectory());
    QFile file(CrashLogger::crashLogDirectory() + "/crash_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".log");
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << "Crash signal: " << signal << "\n";
        out << "Time: " << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
        out << "Application: " << QCoreApplication::applicationName() << "\n";
    }
    std::_Exit(128 + signal);
}

CrashLogger::CrashLogger(QObject *parent)
    : QObject(parent)
{
}

void CrashLogger::install()
{
    std::signal(SIGSEGV, writeCrashSignal);
    std::signal(SIGABRT, writeCrashSignal);
    std::signal(SIGFPE, writeCrashSignal);
    std::signal(SIGILL, writeCrashSignal);
    writeRuntimeMarker(QStringLiteral("应用启动"));
}

QString CrashLogger::crashLogDirectory()
{
    return QCoreApplication::applicationDirPath() + "/crash";
}

void CrashLogger::writeRuntimeMarker(const QString &message)
{
    QDir().mkpath(crashLogDirectory());
    QFile file(crashLogDirectory() + "/runtime.log");
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ") << message << "\n";
}
