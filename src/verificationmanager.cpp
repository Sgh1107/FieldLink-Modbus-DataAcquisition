#include "verificationmanager.h"
#include <QFile>
#include <QTextStream>

VerificationManager::VerificationManager(QObject *parent)
    : QObject(parent)
{
    generateDefaultPlan();
}

void VerificationManager::addResult(const QString &category, const QString &name, const QString &method, const QString &result)
{
    VerificationItem item;
    item.category = category;
    item.name = name;
    item.method = method;
    item.result = result;
    item.time = QDateTime::currentDateTime();
    m_items.append(item);
}

void VerificationManager::generateDefaultPlan()
{
    if (!m_items.isEmpty())
        return;
    const QString pending = tr("Pending / recordable");
    addResult(tr("Communication test"), tr("Modbus simulator test"),
              tr("Cover function codes 01/02/03/04/0F/10/17 with a simulated slave"), pending);
    addResult(tr("Soak test"), tr("24/72/168-hour soak test"),
              tr("Continuous polling, writes, history archiving and export monitoring"), pending);
    addResult(tr("Memory test"), tr("Memory leak test"),
              tr("Long-running polling to observe memory curves and object release"), pending);
    addResult(tr("Stress test"), tr("High-concurrency polling test"),
              tr("Multi-task, multi-point and multi-device polling stress validation"), pending);
    addResult(tr("Exception test"), tr("Abnormal disconnect test"),
              tr("TCP disconnect, serial unplug/replug and auto recovery from unresponsive slaves"), pending);
    addResult(tr("Database test"), tr("Database write stress test"),
              tr("Validate history archiving, cleanup policy and disk anomalies"), pending);
}

bool VerificationManager::exportReport(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out << "# 工业级验证测试报告\n\n";
    for (const auto &item : m_items) {
        out << "- [" << item.category << "] " << item.name << "\n";
        out << "  - 方法: " << item.method << "\n";
        out << "  - 结果: " << item.result << "\n";
        out << "  - 时间: " << item.time.toString("yyyy-MM-dd HH:mm:ss") << "\n";
    }
    return true;
}

QVector<VerificationItem> VerificationManager::items() const
{
    return m_items;
}
