QT += core network gui
QT -= widgets
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = plugin_ipc_test
TEMPLATE = app

INCLUDEPATH += $$PWD/.. $$PWD/../include/common $$PWD/../include/protocol $$PWD/../plugin_api

# protobuf 代码生成 + 链接(见 ipc/ipc.pri)
include(../ipc/ipc.pri)

SOURCES += plugin_ipc_test.cpp \
    ../ipc/plugin_serialization.cpp

HEADERS += ../ipc/plugin_ipc.h \
    ../ipc/plugin_serialization.h
