
#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QSerialPort>
#include <QSettings>

QT_BEGIN_NAMESPACE

namespace Ui {
class SettingsDialog;
}

QT_END_NAMESPACE

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    struct Settings {
        int parity = QSerialPort::EvenParity;
        int baud = QSerialPort::Baud19200;
        int dataBits = QSerialPort::Data8;
        int stopBits = QSerialPort::OneStop;
        int responseTime = 1000;
        int numberOfRetries = 3;
    };

    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog();

    Settings settings() const;
    void setSettings(const Settings &s);
    void loadFromQSettings(QSettings &qs);
    void saveToQSettings(QSettings &qs) const;

private:
    // 校验位下拉框与 QSerialPort::Parity 枚举的显式双向映射。
    // 不能用"下标 + 1"：下拉框顺序是 No/Even/Odd/Space/Mark，
    // 而枚举是 No=0/Odd=1/Even=2/Space=3/Mark=4，算术映射会错位。
    static int parityFromComboIndex(int index);
    static int comboIndexFromParity(int parity);

    Settings m_settings;
    Ui::SettingsDialog *ui;
};

#endif // SETTINGSDIALOG_H
