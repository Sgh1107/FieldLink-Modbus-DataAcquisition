#ifndef DEVICETEMPLATE_H
#define DEVICETEMPLATE_H

#include <QObject>
#include <QVector>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QModbusDataUnit>

struct RegisterDefinition {
    QString name;
    QModbusDataUnit::RegisterType registerType;
    int address;
    int count;
    QString dataType; // "uint16", "int16", "uint32", "int32", "float32", "ascii"
    QString byteOrder; // "ABCD", "DCBA", "BADC", "CDAB"
    double scale;
    double offset;
    QString unit;
    QString description;
};

struct DeviceTemplate {
    int id;
    QString name;
    QString manufacturer;
    QString model;
    QString description;
    QVector<RegisterDefinition> registers;
};

class DeviceTemplateManager : public QObject
{
    Q_OBJECT

public:
    explicit DeviceTemplateManager(QObject *parent = nullptr);

    int addTemplate(const DeviceTemplate &tmpl);
    void removeTemplate(int templateId);
    void updateTemplate(const DeviceTemplate &tmpl);
    DeviceTemplate getTemplate(int templateId) const;
    QVector<DeviceTemplate> allTemplates() const;

    bool saveToFile(const QString &filePath) const;
    bool loadFromFile(const QString &filePath);

    static QJsonObject templateToJson(const DeviceTemplate &tmpl);
    static DeviceTemplate templateFromJson(const QJsonObject &obj);

private:
    QVector<DeviceTemplate> m_templates;
    int m_nextId;
};

#endif // DEVICETEMPLATE_H
