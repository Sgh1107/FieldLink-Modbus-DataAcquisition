#ifndef VERIFICATIONMANAGER_H
#define VERIFICATIONMANAGER_H

#include <QObject>
#include <QStringList>
#include <QDateTime>

struct VerificationItem {
    QString category;
    QString name;
    QString method;
    QString result;
    QDateTime time;
};

class VerificationManager : public QObject
{
    Q_OBJECT

public:
    explicit VerificationManager(QObject *parent = nullptr);

    void addResult(const QString &category, const QString &name, const QString &method, const QString &result);
    void generateDefaultPlan();
    bool exportReport(const QString &filePath) const;
    QVector<VerificationItem> items() const;

private:
    QVector<VerificationItem> m_items;
};

#endif // VERIFICATIONMANAGER_H
