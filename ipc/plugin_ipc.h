/// @file plugin_ipc.h
/// @brief 插件 IPC 协议定义(主程序 ↔ bplc-plugin-host,protobuf 信封版)
/// @details 传输:QLocalSocket;帧格式:[quint32 总长][Envelope protobuf 字节]。
///          旧格式 [quint32 总长][quint8 类型][QDataStream 载荷] 已废弃:
///          kApiVersion 1 → 2,kStreamVersion 与 MsgType 一并删除;
///          消息类型改由 Envelope.body 的 oneof 区分,关联一律走
///          Envelope.correlation_id(体内不再带 seq)。
///          信封级错误(Status=ERROR + error)与消息级错误(ErrorMsg)分开:
///          前者是传输/协议层失败,后者是插件业务层异常。
#ifndef BPLC_PLUGIN_IPC_H
#define BPLC_PLUGIN_IPC_H

#include <QCoreApplication>
#include <QString>
#include <QtGlobal>

namespace plugin_ipc {

/// @brief 插件 API 版本(主程序与 host 握手时校验,不一致则拒绝)
///        1=QDataStream 线格式(已废弃);2=protobuf Envelope(现行)
constexpr int kApiVersion = 2;

/// @brief proto schema 版本(Hello 协商用;与 plugin_ipc.proto 的
///        proto_schema_version 对应,本 schema = 1)
constexpr int kProtoSchemaVersion = 1;

/// @brief 本机 socket 名(主程序按插件 id 生成唯一名)
/// @param plugin_id 插件 id
inline QString socketName(const QString& plugin_id) {
    return QStringLiteral("bplc_plugin_%1_%2")
        .arg(plugin_id)
        .arg(QCoreApplication::applicationPid());
}

} // namespace plugin_ipc

#endif // BPLC_PLUGIN_IPC_H
