# 框架独立编译冒烟测试(不链接成可执行文件,只验证编译通过)
TEMPLATE = app
TARGET = framework_test
QT += core gui widgets network qml
CONFIG += c++17 console

# 框架本体
include($$PWD/plugin.pri)
include($$PWD/app_integration/app_integration.pri)
include($$PWD/market/market.pri)
include($$PWD/lua_bundle.pri)

# 测试入口(空 main,只为触发编译)
SOURCES += $$PWD/tests/framework_smoke_main.cpp
