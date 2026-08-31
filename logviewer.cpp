#include "logviewer.h"
#include <QFile>
#include <QTextStream>
#include <QFileDialog>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QCoreApplication>
#include <QTextCursor>

LogViewer::LogViewer(QWidget *parent)
    : QDialog(parent)
    , m_currentFilter(LogLevel::All)
{
    setupUi();
    setWindowTitle("日志查看器");
    resize(700, 500);
}

void LogViewer::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    auto *topLayout = new QHBoxLayout();
    topLayout->addWidget(new QLabel("日期:"));
    m_dateSelector = new QDateEdit(QDate::currentDate());
    m_dateSelector->setCalendarPopup(true);
    topLayout->addWidget(m_dateSelector);

    topLayout->addWidget(new QLabel("级别:"));
    m_levelFilter = new QComboBox();
    m_levelFilter->addItems({"All", "DEBUG", "INFO", "WARNING", "ERROR"});
    topLayout->addWidget(m_levelFilter);

    m_refreshBtn = new QPushButton("刷新");
    topLayout->addWidget(m_refreshBtn);

    m_clearBtn = new QPushButton("清空");
    topLayout->addWidget(m_clearBtn);

    m_exportBtn = new QPushButton("导出");
    topLayout->addWidget(m_exportBtn);

    m_autoScrollCheck = new QCheckBox("自动滚动");
    m_autoScrollCheck->setChecked(true);
    topLayout->addWidget(m_autoScrollCheck);

    topLayout->addStretch();
    mainLayout->addLayout(topLayout);

    m_logDisplay = new QPlainTextEdit();
    m_logDisplay->setReadOnly(true);
    m_logDisplay->setMaximumBlockCount(10000);
    mainLayout->addWidget(m_logDisplay);

    connect(m_refreshBtn, &QPushButton::clicked, this, &LogViewer::refreshLog);
    connect(m_clearBtn, &QPushButton::clicked, this, &LogViewer::clearDisplay);
    connect(m_exportBtn, &QPushButton::clicked, this, &LogViewer::exportLog);
    connect(m_levelFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LogViewer::filterByLevel);
    connect(m_dateSelector, &QDateEdit::dateChanged, this, [this]() { loadLogFile(); });
}

void LogViewer::setLogDirectory(const QString &dir)
{
    m_logDirectory = dir;
    loadLogFile();
}

void LogViewer::loadLogFile()
{
    m_allLogLines.clear();
    m_logDisplay->clear();

    QString dateStr = m_dateSelector->date().toString("yyyyMMdd");
    QString filePath = m_logDirectory + "/" + dateStr + ".log";

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_logDisplay->setPlainText("无法打开日志文件: " + filePath);
        return;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        m_allLogLines.append(in.readLine());
    }
    file.close();

    filterByLevel(m_levelFilter->currentIndex());
}

void LogViewer::refreshLog()
{
    loadLogFile();
}

void LogViewer::filterByLevel(int levelIndex)
{
    m_currentFilter = static_cast<LogLevel>(levelIndex == 0 ? 4 : levelIndex - 1);
    m_logDisplay->clear();

    for (const QString &line : m_allLogLines) {
        if (matchesFilter(line)) {
            m_logDisplay->appendPlainText(line);
        }
    }

    if (m_autoScrollCheck->isChecked()) {
        m_logDisplay->moveCursor(QTextCursor::End);
    }
}

bool LogViewer::matchesFilter(const QString &line) const
{
    if (m_currentFilter == LogLevel::All)
        return true;

    QString levelStr = levelToString(m_currentFilter);
    return line.contains("[" + levelStr + "]");
}

void LogViewer::clearDisplay()
{
    m_logDisplay->clear();
}

void LogViewer::exportLog()
{
    QString fileName = QFileDialog::getSaveFileName(this, "导出日志",
        QDir::homePath() + "/modbus_log_" + m_dateSelector->date().toString("yyyyMMdd") + ".txt",
        "Text Files (*.txt);;All Files (*)");

    if (fileName.isEmpty()) return;

    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << m_logDisplay->toPlainText();
        file.close();
    }
}

void LogViewer::appendLog(const QString &message, LogLevel level)
{
    QString levelStr = levelToString(level);
    QString fullLine = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                       + " [" + levelStr + "] " + message;
    m_allLogLines.append(fullLine);

    if (matchesFilter(fullLine)) {
        m_logDisplay->appendPlainText(fullLine);
        if (m_autoScrollCheck->isChecked()) {
            m_logDisplay->moveCursor(QTextCursor::End);
        }
    }
}

QString LogViewer::levelToString(LogLevel level) const
{
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
    default: return "ALL";
    }
}

LogLevel LogViewer::stringToLevel(const QString &str) const
{
    if (str == "DEBUG") return LogLevel::Debug;
    if (str == "INFO") return LogLevel::Info;
    if (str == "WARNING") return LogLevel::Warning;
    if (str == "ERROR") return LogLevel::Error;
    return LogLevel::All;
}
