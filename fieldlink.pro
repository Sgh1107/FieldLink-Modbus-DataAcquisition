QT += serialbus serialport widgets network sql qml
requires(qtConfig(combobox))

TARGET = fieldlink
TEMPLATE = app
CONFIG += c++11

# ---- 界面翻译：lrelease 构建时把 .ts 编译为 .qm 并嵌入资源 :/i18n ----
TRANSLATIONS += translations/fieldlink_zh_CN.ts
CONFIG += lrelease embed_translations

SOURCES += main.cpp\
        mainwindow.cpp \
        mainwindow_advanced.cpp \
        settingsdialog.cpp \
        writeregistermodel.cpp \
        dataparser.cpp \
        logviewer.cpp \
        pollmanager.cpp \
        dataexporter.cpp \
        realtimechart.cpp \
        devicemanager.cpp \
        devicetemplate.cpp \
        scriptengine.cpp \
        remoteserver.cpp \
        historydata.cpp \
        pluginmanager.cpp \
        alarmmanager.cpp \
        batchtaskmanager.cpp \
        dashboard.cpp \
        configprofile.cpp \
        reliabilitymanager.cpp \
        securitymanager.cpp \
        pointmodel.cpp \
        verificationmanager.cpp \
        deliverymanager.cpp \
        crashlogger.cpp

HEADERS  += mainwindow.h \
         settingsdialog.h \
        writeregistermodel.h \
        dataparser.h \
        logviewer.h \
        pollmanager.h \
        dataexporter.h \
        realtimechart.h \
        devicemanager.h \
        devicetemplate.h \
        scriptengine.h \
        remoteserver.h \
        historydata.h \
        plugininterface.h \
        pluginmanager.h \
        alarmmanager.h \
        batchtaskmanager.h \
        dashboard.h \
        configprofile.h \
        reliabilitymanager.h \
        securitymanager.h \
        pointmodel.h \
        verificationmanager.h \
        deliverymanager.h \
        crashlogger.h \
        thememanager.h

FORMS    += mainwindow.ui \
         settingsdialog.ui

RESOURCES += \
    fieldlink.qrc

target.path = $$[QT_INSTALL_EXAMPLES]/serialbus/modbus/master
INSTALLS += target
