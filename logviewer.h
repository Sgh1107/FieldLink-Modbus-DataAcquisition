#ifndef LOGVIEWER_H
#define LOGVIEWER_H

#include <QDialog>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QDateEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>

enum class LogLevel {
    Debug = 0,
    Info,
    Warning,
    Error,
    All
};

class LogViewer : public QDialog
{
    Q_OBJECT

public:
    explicit LogViewer(QWidget *parent = nullptr);
    void setLogDirectory(const QString &dir);
    void appendLog(const QString &message, LogLevel level = LogLevel::Info);

public slots:
    void loadLogFile();
    void refreshLog();
    void filterByLevel(int levelIndex);
    void clearDisplay();
    void exportLog();

private:
    void setupUi();
    QString levelToString(LogLevel level) const;
    LogLevel stringToLevel(const QString &str) const;
    bool matchesFilter(const QString &line) const;

    QPlainTextEdit *m_logDisplay;
    QComboBox *m_levelFilter;
    QDateEdit *m_dateSelector;
    QPushButton *m_refreshBtn;
    QPushButton *m_clearBtn;
    QPushButton *m_exportBtn;
    QCheckBox *m_autoScrollCheck;
    QString m_logDirectory;
    QStringList m_allLogLines;
    LogLevel m_currentFilter;
};

#endif // LOGVIEWER_H
