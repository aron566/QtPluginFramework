# ipc.pri: 插件 IPC 的 protobuf 代码生成 + 链接
# 行为:qmake 时调 protoc 由 plugin_ipc.proto 生成 plugin_ipc.pb.cc/.pb.h
# (生成物在 ipc/ 目录下,不提交版本库,见 .gitignore;proto 是唯一事实来源)。
# 要求:protobuf-compiler(protoc)与 libprotobuf-dev 在 PATH/链接路径中;
# Windows 侧需自行装 protobuf(vcpkg)并保证 protoc 可用,链接名见下方 win32 行。
# 本文件被 host/test/plugin.pri 各自 include,重复 include 无害
# (SOURCES/HEADERS 去重由 qmake 处理,protoc 多跑一次结果相同)。

IPC_PROTO = $$PWD/plugin_ipc.proto
IPC_GEN_CC = $$PWD/plugin_ipc.pb.cc
IPC_GEN_H = $$PWD/plugin_ipc.pb.h

message("ipc: generating protobuf code from $$IPC_PROTO ...")
system(protoc --cpp_out=$$PWD --proto_path=$$PWD $$IPC_PROTO)
!exists($$IPC_GEN_CC): error("ipc: protoc failed to generate $$IPC_GEN_CC (is protobuf-compiler installed and protoc in PATH?)")
!exists($$IPC_GEN_H): error("ipc: protoc failed to generate $$IPC_GEN_H")

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS += $$IPC_GEN_H
SOURCES += $$IPC_GEN_CC

unix: LIBS += -lprotobuf
win32: LIBS += -llibprotobuf
