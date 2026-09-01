QT += serialbus serialport widgets network sql qml
requires(qtConfig(combobox))

TARGET = fieldlink
TEMPLATE = app
CONFIG += c++11

# ---- 源码目录：头文件在 header/，源文件在 src/ ----
INCLUDEPATH += $$PWD/header
DEPENDPATH += $$PWD/header

# ---- 界面翻译：lrelease 构建时把 .ts 编译为 .qm 并嵌入资源 :/i18n ----
TRANSLATIONS += translations/fieldlink_zh_CN.ts
CONFIG += lrelease embed_translations

SOURCES += src/main.cpp\
        src/mainwindow.cpp \
        src/mainwindow_advanced.cpp \
        src/settingsdialog.cpp \
        src/writeregistermodel.cpp \
        src/dataparser.cpp \
        src/logviewer.cpp \
        src/pollmanager.cpp \
        src/dataexporter.cpp \
        src/realtimechart.cpp \
        src/devicemanager.cpp \
        src/devicetemplate.cpp \
        src/scriptengine.cpp \
        src/remoteserver.cpp \
        src/historydata.cpp \
        src/pluginmanager.cpp \
        src/alarmmanager.cpp \
        src/batchtaskmanager.cpp \
        src/dashboard.cpp \
        src/configprofile.cpp \
        src/reliabilitymanager.cpp \
        src/securitymanager.cpp \
        src/pointmodel.cpp \
        src/verificationmanager.cpp \
        src/deliverymanager.cpp \
        src/crashlogger.cpp \
        src/mqttclient.cpp

HEADERS  += header/mainwindow.h \
         header/settingsdialog.h \
        header/writeregistermodel.h \
        header/dataparser.h \
        header/logviewer.h \
        header/pollmanager.h \
        header/dataexporter.h \
        header/realtimechart.h \
        header/devicemanager.h \
        header/devicetemplate.h \
        header/scriptengine.h \
        header/remoteserver.h \
        header/historydata.h \
        header/plugininterface.h \
        header/pluginmanager.h \
        header/alarmmanager.h \
        header/batchtaskmanager.h \
        header/dashboard.h \
        header/configprofile.h \
        header/reliabilitymanager.h \
        header/securitymanager.h \
        header/pointmodel.h \
        header/verificationmanager.h \
        header/deliverymanager.h \
        header/crashlogger.h \
        header/thememanager.h \
        header/mqttclient.h

FORMS    += mainwindow.ui \
         settingsdialog.ui

RESOURCES += \
    fieldlink.qrc

target.path = $$[QT_INSTALL_EXAMPLES]/serialbus/modbus/master
INSTALLS += target
