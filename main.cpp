// main.cpp
// 应用程序入口文件

#include "mainwindow.h"  // 包含主窗口头文件
#include "crashlogger.h"
#include "thememanager.h"  // 主题管理器（Fusion + Palette + QSS 三层主题方案）

#include <QApplication>      // Qt应用程序框架核心类
#include <QLoggingCategory>  // Qt日志分类管理类
#include <QStyleFactory>     // 样式工厂（Fusion 基础样式）
#include <QSettings>         // 读取上次使用的主题
#include <QFont>             // 应用全局字体

int main(int argc, char *argv[])
{
    // 设置Qt Modbus模块的日志过滤规则
    // 这里启用所有qt.modbus前缀的日志输出（调试时有用）
    QLoggingCategory::setFilterRules(QStringLiteral("qt.modbus* = true"));

    // 创建Qt应用程序对象
    // argc: 命令行参数数量
    // argv: 命令行参数数组
    QApplication a(argc, argv);
    a.setApplicationName("fieldlink");
    a.setApplicationVersion("2.1.0");
    CrashLogger::install();

    // ---------- 界面主题初始化 ----------
    // 1) 统一使用 Fusion 基础样式，避免 Windows 原生样式在深色主题下出现
    //    白底控件与深色界面割裂的问题；
    // 2) 全局使用“微软雅黑 UI”，中文界面在小字号下更清晰；
    // 3) 主题默认深色工业风，用户切换后会通过 QSettings 持久化记忆。
    a.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    a.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    {
        QSettings settings;  // 与 MainWindow::m_appSettings 使用同一份配置
        const QString theme = settings.value(
            QStringLiteral("ui/theme"), QStringLiteral("dark")).toString();
        ThemeManager::applyTheme(theme);
    }

    // 创建应用程序主窗口
    MainWindow w;

    // 设置应用程序窗口图标
    // ":/images/logo.ico" 是资源文件路径（需在.qrc文件中定义）
    w.setWindowIcon(QIcon(":/images/logo.ico"));

    // 显示主窗口
    w.show();

    // 进入Qt主事件循环
    // 此调用会阻塞直到应用程序退出
    return a.exec();

    /* 程序执行流程说明：
     * 1. 初始化Qt应用程序框架
     * 2. 配置Modbus模块日志输出
     * 3. 创建主窗口对象
     * 4. 设置窗口图标
     * 5. 显示主窗口
     * 6. 进入事件循环（处理用户输入、定时器、网络事件等）
     * 7. 当最后一个窗口关闭时，exec()返回，程序结束
     */
}

