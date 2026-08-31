
#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include <QComboBox>
#include <QList>

SettingsDialog::SettingsDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);

    setWindowTitle("通信参数设置");
    resize(560, 500);
    setMinimumSize(520, 460);

    ui->groupBox->setTitle("  串口参数 / Serial Parameters  ");
    ui->label->setText("响应超时:");
    ui->label_2->setText("校验位:");
    ui->label_3->setText("波特率:");
    ui->label_4->setText("数据位:");
    ui->label_5->setText("停止位:");
    ui->label_6->setText("重试次数:");
    ui->applyButton->setText("应用参数");

    ui->gridLayout->setContentsMargins(24, 24, 24, 24);
    ui->gridLayout->setHorizontalSpacing(14);
    ui->gridLayout->setVerticalSpacing(14);
    ui->gridLayout_2->setContentsMargins(20, 26, 20, 20);
    ui->gridLayout_2->setHorizontalSpacing(18);
    ui->gridLayout_2->setVerticalSpacing(16);

    const QList<QComboBox*> combos = {ui->parityCombo, ui->baudCombo, ui->dataBitsCombo, ui->stopBitsCombo};
    for (QComboBox *combo : combos) {
        combo->setMinimumHeight(42);
        combo->setMinimumWidth(220);
    }
    ui->timeoutSpinner->setMinimumHeight(42);
    ui->retriesSpinner->setMinimumHeight(42);
    ui->timeoutSpinner->setMinimumWidth(150);
    ui->retriesSpinner->setMinimumWidth(120);
    ui->applyButton->setMinimumHeight(46);
    ui->applyButton->setMinimumWidth(220);

    // 界面样式跟随全局主题（见 thememanager.h / style/*.qss），
    // “应用”按钮的主色强调在各主题 QSS 中以 QPushButton#applyButton 定义。

    // 设置对话框标题
    setWindowTitle("通信参数设置");


    ui->parityCombo->setCurrentIndex(1);
    ui->baudCombo->setCurrentText(QString::number(m_settings.baud));
    ui->dataBitsCombo->setCurrentText(QString::number(m_settings.dataBits));
    ui->stopBitsCombo->setCurrentText(QString::number(m_settings.stopBits));
    ui->timeoutSpinner->setValue(m_settings.responseTime);
    ui->retriesSpinner->setValue(m_settings.numberOfRetries);

    connect(ui->applyButton, &QPushButton::clicked, [this]() {
        m_settings.parity = ui->parityCombo->currentIndex();
        if (m_settings.parity > 0)
            m_settings.parity++;
        m_settings.baud = ui->baudCombo->currentText().toInt();
        m_settings.dataBits = ui->dataBitsCombo->currentText().toInt();
        m_settings.stopBits = ui->stopBitsCombo->currentText().toInt();
        m_settings.responseTime = ui->timeoutSpinner->value();
        m_settings.numberOfRetries = ui->retriesSpinner->value();

        hide();
    });
}

SettingsDialog::~SettingsDialog()
{
    delete ui;
}

SettingsDialog::Settings SettingsDialog::settings() const
{
    return m_settings;
}

void SettingsDialog::setSettings(const Settings &s)
{
    m_settings = s;
    ui->parityCombo->setCurrentIndex(m_settings.parity > 0 ? m_settings.parity - 1 : 0);
    ui->baudCombo->setCurrentText(QString::number(m_settings.baud));
    ui->dataBitsCombo->setCurrentText(QString::number(m_settings.dataBits));
    ui->stopBitsCombo->setCurrentText(QString::number(m_settings.stopBits));
    ui->timeoutSpinner->setValue(m_settings.responseTime);
    ui->retriesSpinner->setValue(m_settings.numberOfRetries);
}

void SettingsDialog::loadFromQSettings(QSettings &qs)
{
    Settings s;
    s.parity = qs.value("serial/parity", s.parity).toInt();
    s.baud = qs.value("serial/baud", s.baud).toInt();
    s.dataBits = qs.value("serial/dataBits", s.dataBits).toInt();
    s.stopBits = qs.value("serial/stopBits", s.stopBits).toInt();
    s.responseTime = qs.value("serial/responseTime", s.responseTime).toInt();
    s.numberOfRetries = qs.value("serial/numberOfRetries", s.numberOfRetries).toInt();
    setSettings(s);
}

void SettingsDialog::saveToQSettings(QSettings &qs) const
{
    qs.setValue("serial/parity", m_settings.parity);
    qs.setValue("serial/baud", m_settings.baud);
    qs.setValue("serial/dataBits", m_settings.dataBits);
    qs.setValue("serial/stopBits", m_settings.stopBits);
    qs.setValue("serial/responseTime", m_settings.responseTime);
    qs.setValue("serial/numberOfRetries", m_settings.numberOfRetries);
}
