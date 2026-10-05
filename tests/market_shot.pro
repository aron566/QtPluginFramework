QT += core gui widgets network
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = market_shot
TEMPLATE = app

INCLUDEPATH += $$PWD/../market $$PWD/../plugin_api $$PWD/../../common \
    $$PWD/../../../3rdparty/miniz

SOURCES += market_shot.cpp \
    ../market/plugin_market.cpp \
    ../market/market_dialog.cpp \
    ../../common/i18n.cpp \
    $$PWD/../../../3rdparty/miniz/miniz.c \
    $$PWD/../../../3rdparty/miniz/miniz_tdef.c \
    $$PWD/../../../3rdparty/miniz/miniz_tinfl.c \
    $$PWD/../../../3rdparty/miniz/miniz_zip.c

HEADERS += ../market/plugin_market.h ../market/market_dialog.h
