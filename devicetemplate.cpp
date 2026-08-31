#include "devicetemplate.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

DeviceTemplateManager::DeviceTemplateManager(QObject *parent)
    : QObject(parent)
    , m_nextId(1)
{
}

int DeviceTemplateManager::addTemplate(const DeviceTemplate &tmpl)
{
    DeviceTemplate t = tmpl;
    t.id = m_nextId++;
    m_templates.append(t);
    return t.id;
}

void DeviceTemplateManager::removeTemplate(int templateId)
{
    for (int i = 0; i < m_templates.size(); ++i) {
        if (m_templates[i].id == templateId) {
            m_templates.removeAt(i);
            break;
        }
    }
}

void DeviceTemplateManager::updateTemplate(const DeviceTemplate &tmpl)
{
    for (auto &t : m_templates) {
        if (t.id == tmpl.id) {
            t = tmpl;
            break;
        }
    }
}

DeviceTemplate DeviceTemplateManager::getTemplate(int templateId) const
{
    for (const auto &t : m_templates) {
        if (t.id == templateId)
            return t;
    }
    return DeviceTemplate();
}

QVector<DeviceTemplate> DeviceTemplateManager::allTemplates() const
{
    return m_templates;
}

QJsonObject DeviceTemplateManager::templateToJson(const DeviceTemplate &tmpl)
{
    QJsonObject obj;
    obj["name"] = tmpl.name;
    obj["manufacturer"] = tmpl.manufacturer;
    obj["model"] = tmpl.model;
    obj["description"] = tmpl.description;

    QJsonArray regsArray;
    for (const auto &reg : tmpl.registers) {
        QJsonObject regObj;
        regObj["name"] = reg.name;
        regObj["registerType"] = static_cast<int>(reg.registerType);
        regObj["address"] = reg.address;
        regObj["count"] = reg.count;
        regObj["dataType"] = reg.dataType;
        regObj["byteOrder"] = reg.byteOrder;
        regObj["scale"] = reg.scale;
        regObj["offset"] = reg.offset;
        regObj["unit"] = reg.unit;
        regObj["description"] = reg.description;
        regsArray.append(regObj);
    }
    obj["registers"] = regsArray;
    return obj;
}

DeviceTemplate DeviceTemplateManager::templateFromJson(const QJsonObject &obj)
{
    DeviceTemplate tmpl;
    tmpl.name = obj["name"].toString();
    tmpl.manufacturer = obj["manufacturer"].toString();
    tmpl.model = obj["model"].toString();
    tmpl.description = obj["description"].toString();

    QJsonArray regsArray = obj["registers"].toArray();
    for (const auto &val : regsArray) {
        QJsonObject regObj = val.toObject();
        RegisterDefinition reg;
        reg.name = regObj["name"].toString();
        reg.registerType = static_cast<QModbusDataUnit::RegisterType>(regObj["registerType"].toInt());
        reg.address = regObj["address"].toInt();
        reg.count = regObj["count"].toInt();
        reg.dataType = regObj["dataType"].toString();
        reg.byteOrder = regObj["byteOrder"].toString();
        reg.scale = regObj["scale"].toDouble(1.0);
        reg.offset = regObj["offset"].toDouble(0.0);
        reg.unit = regObj["unit"].toString();
        reg.description = regObj["description"].toString();
        tmpl.registers.append(reg);
    }
    return tmpl;
}

bool DeviceTemplateManager::saveToFile(const QString &filePath) const
{
    QJsonArray arr;
    for (const auto &tmpl : m_templates) {
        arr.append(templateToJson(tmpl));
    }

    QJsonDocument doc(arr);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool DeviceTemplateManager::loadFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isArray()) return false;

    m_templates.clear();
    QJsonArray arr = doc.array();
    for (const auto &val : arr) {
        DeviceTemplate tmpl = templateFromJson(val.toObject());
        tmpl.id = m_nextId++;
        m_templates.append(tmpl);
    }
    return true;
}
