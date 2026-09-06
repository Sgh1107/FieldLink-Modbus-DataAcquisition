# fieldlink 单元/集成测试（无 GUI，覆盖核心逻辑模块）
# 构建方式：
#   mkdir build\tests && cd build\tests
#   qmake ..\..\tests\tests.pro && mingw32-make
# 运行：fieldlink_tests.exe（退出码 0 = 全部通过）

QT += core network sql serialbus
QT -= gui
CONFIG += c++11 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = fieldlink_tests

INCLUDEPATH += $$PWD/../header
DEPENDPATH += $$PWD/../header

SOURCES += \
    master_tests.cpp \
    ../src/alarmmanager.cpp \
    ../src/securitymanager.cpp \
    ../src/reliabilitymanager.cpp \
    ../src/mqttclient.cpp \
    ../src/dataexporter.cpp \
    ../src/pollmanager.cpp \
    ../src/devicemanager.cpp \
    ../src/historydata.cpp \
    ../src/batchtaskmanager.cpp \
    ../src/dataparser.cpp

HEADERS += \
    ../header/alarmmanager.h \
    ../header/securitymanager.h \
    ../header/reliabilitymanager.h \
    ../header/mqttclient.h \
    ../header/dataexporter.h \
    ../header/pollmanager.h \
    ../header/devicemanager.h \
    ../header/historydata.h \
    ../header/batchtaskmanager.h \
    ../header/dataparser.h
