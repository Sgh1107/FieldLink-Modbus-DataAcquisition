#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H
// =====================================================================
// ThemeManager - 应用主题管理器（header-only，无需 moc）
//
// 采用业界成熟的“三层”主题方案，保证深/浅两套主题下所有原生控件都正确渲染：
//   1) Fusion 基础样式：跨 Windows 原生样式的一致几何与自绘元素（旋转框箭头、
//      复选框指示器等），避免原生 Vista 样式在深色下出现白底黑字割裂；
//   2) QPalette 全局配色：解决绝大多数控件的文本/背景/禁用态颜色，包括
//      QMenu、QComboBox 弹层、QSpinBox 箭头等 QSS 难以覆盖的角落；
//   3) QSS 细节增强：卡片式 GroupBox、圆角按钮、现代滚动条、菜单悬浮态等。
//
// 主题 QSS 资源：:/style/dark.qss 与 :/style/light.qss（见 fieldlink.qrc）。
// =====================================================================

#include <QCoreApplication>
#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QStyleFactory>
#include <QColor>

namespace ThemeManager {

// 可用主题 id 列表（同时作为资源文件名与 QSettings 持久化键值）
inline QStringList availableThemes()
{
    return QStringList() << QStringLiteral("dark") << QStringLiteral("light");
}

inline QString themeDisplayName(const QString &themeId)
{
    if (themeId == QLatin1String("light"))
        return QStringLiteral("浅色现代风");
    return QStringLiteral("深色工业风");
}

inline QString normalizedThemeId(const QString &themeId)
{
    return availableThemes().contains(themeId) ? themeId : QStringLiteral("dark");
}

// 构建主题对应的 QPalette。Fusion 会用它绘制所有未被子类化的原生元素。
inline QPalette buildPalette(const QString &themeId)
{
    QPalette pal;
    if (themeId == QLatin1String("light")) {
        // ---- 浅色现代风 v2（靛蓝高亮） ----
        pal.setColor(QPalette::Window,          QColor(0xF2, 0xF5, 0xFA));
        pal.setColor(QPalette::WindowText,      QColor(0x1F, 0x29, 0x37));
        pal.setColor(QPalette::Base,            QColor(0xFF, 0xFF, 0xFF));
        pal.setColor(QPalette::AlternateBase,   QColor(0xF7, 0xF9, 0xFC));
        pal.setColor(QPalette::Text,            QColor(0x1F, 0x29, 0x37));
        pal.setColor(QPalette::Button,          QColor(0xFF, 0xFF, 0xFF));
        pal.setColor(QPalette::ButtonText,      QColor(0x1F, 0x29, 0x37));
        pal.setColor(QPalette::ToolTipBase,     QColor(0xFF, 0xFF, 0xFF));
        pal.setColor(QPalette::ToolTipText,     QColor(0x1F, 0x29, 0x37));
        pal.setColor(QPalette::BrightText,      Qt::white);
        pal.setColor(QPalette::Highlight,       QColor(0x4F, 0x46, 0xE5));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Link,            QColor(0x4F, 0x46, 0xE5));
        pal.setColor(QPalette::PlaceholderText, QColor(0x94, 0xA3, 0xB8));
        pal.setColor(QPalette::Disabled, QPalette::WindowText,  QColor(0xAE, 0xB9, 0xC9));
        pal.setColor(QPalette::Disabled, QPalette::Text,        QColor(0xAE, 0xB9, 0xC9));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText,  QColor(0xAE, 0xB9, 0xC9));
        pal.setColor(QPalette::Disabled, QPalette::Base,        QColor(0xF3, 0xF5, 0xF9));
        pal.setColor(QPalette::Disabled, QPalette::Highlight,   QColor(0xC7, 0xD2, 0xFE));
        pal.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(0x64, 0x74, 0x8B));
    } else {
        // ---- 深色工业风 v2（靛蓝高亮） ----
        pal.setColor(QPalette::Window,          QColor(0x10, 0x15, 0x1F));
        pal.setColor(QPalette::WindowText,      QColor(0xE8, 0xED, 0xF7));
        pal.setColor(QPalette::Base,            QColor(0x0C, 0x10, 0x18));
        pal.setColor(QPalette::AlternateBase,   QColor(0x12, 0x1A, 0x2B));
        pal.setColor(QPalette::Text,            QColor(0xE8, 0xED, 0xF7));
        pal.setColor(QPalette::Button,          QColor(0x1B, 0x24, 0x34));
        pal.setColor(QPalette::ButtonText,      QColor(0xDB, 0xE4, 0xF5));
        pal.setColor(QPalette::ToolTipBase,     QColor(0x0B, 0x0F, 0x18));
        pal.setColor(QPalette::ToolTipText,     QColor(0xE8, 0xED, 0xF7));
        pal.setColor(QPalette::BrightText,      Qt::white);
        pal.setColor(QPalette::Highlight,       QColor(0x63, 0x66, 0xF1));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Link,            QColor(0x22, 0xD3, 0xEE));
        pal.setColor(QPalette::PlaceholderText, QColor(0x64, 0x74, 0x8B));
        pal.setColor(QPalette::Disabled, QPalette::WindowText,  QColor(0x4B, 0x58, 0x72));
        pal.setColor(QPalette::Disabled, QPalette::Text,        QColor(0x4B, 0x58, 0x72));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText,  QColor(0x4B, 0x58, 0x72));
        pal.setColor(QPalette::Disabled, QPalette::Base,        QColor(0x10, 0x14, 0x1F));
        pal.setColor(QPalette::Disabled, QPalette::Highlight,   QColor(0x26, 0x30, 0x46));
        pal.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(0x4B, 0x58, 0x72));
    }
    return pal;
}

// 将主题应用到整个应用（Fusion 样式 + 调色板 + QSS）。可在任意时刻重复调用完成切换。
inline bool applyTheme(const QString &themeId)
{
    QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app)
        return false;

    const QString id = normalizedThemeId(themeId);
    app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app->setPalette(buildPalette(id));

    QFile qssFile(QStringLiteral(":/style/%1.qss").arg(id));
    if (!qssFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    app->setStyleSheet(QString::fromUtf8(qssFile.readAll()));
    return true;
}

} // namespace ThemeManager

#endif // THEMEMANAGER_H
