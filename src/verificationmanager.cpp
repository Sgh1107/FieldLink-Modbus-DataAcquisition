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
    addResult("通信测试", "Modbus模拟从站测试", "使用模拟从站覆盖01/02/03/04/0F/10/17功能码", "待执行/可记录");
    addResult("长稳测试", "24/72/168小时长稳测试", "连续轮询、写入、历史归档和导出监控", "待执行/可记录");
    addResult("内存测试", "内存泄露测试", "长时间轮询观察内存曲线与对象释放", "待执行/可记录");
    addResult("压力测试", "高并发轮询测试", "多任务、多点位、多设备轮询压力验证", "待执行/可记录");
    addResult("异常测试", "异常断线测试", "TCP断线、串口拔插、从站无响应自动恢复", "待执行/可记录");
    addResult("数据库测试", "数据库写入压力测试", "历史归档、清理策略、磁盘异常验证", "待执行/可记录");
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
