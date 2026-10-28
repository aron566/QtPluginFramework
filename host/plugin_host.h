/// @file plugin_host.h
/// @brief bplc-plugin-host:插件宿主进程(加载插件,经 IPC 服务主程序)
/// @details 支持多种 runtime(native/js/lua),通过 IPluginBackend 抽象。
///          IPC 为 protobuf Envelope 版(见 ipc/plugin_ipc.h):帧格式
///          [quint32 总长][Envelope protobuf 字节],关联走 correlation_id。
#ifndef BPLC_PLUGIN_HOST_H
#define BPLC_PLUGIN_HOST_H

#include <QObject>
#include <QLocalSocket>
#include <QByteArray>

#include "plugin_manifest.h"
#include "plugin_ipc.h"
#include "plugin_ipc.pb.h"

class IPluginBackend;

/// @brief 插件宿主:加载一个插件,处理主程序的 IPC 请求
class PluginHost : public QObject {
    Q_OBJECT
public:
    explicit PluginHost(const QString& socket_name, const QString& plugin_dir,
                        QObject* parent = nullptr);
    ~PluginHost() override;

    /// @brief 启动:加载插件 + 连接主程序。false=启动失败(调用方退出非0)
    bool start(QString* err);
    /// @brief 运行事件循环直到退出
    int exec();

private slots:
    void on_connected();
    void on_ready_read();
    void on_disconnected();
    void on_socket_error(QLocalSocket::LocalSocketError e);

private:
    void send_envelope(const qtplugin::ipc::Envelope& env);
    void handle_envelope(const qtplugin::ipc::Envelope& env);
    /// @brief 单帧解析(供单条与批条目共用);返回的 ParseResponse 含双写
    ///        accepted 与 result(含 reject_reason),与旧线格式一致
    qtplugin::ipc::ParseResponse do_parse(const qtplugin::ipc::ParseRequest& req);
    void handle_parse_request(quint64 corr, const qtplugin::ipc::ParseRequest& req);
    void handle_parse_batch(quint64 corr, const qtplugin::ipc::ParseBatchRequest& batch);
    void handle_render_request(quint64 corr, const qtplugin::ipc::RenderRequest& req);
    void handle_graphics_event(quint64 corr, const qtplugin::ipc::GraphicsEventMsg& msg);

    QString            m_socket_name;
    PluginManifest     m_manifest;
    IPluginBackend*    m_backend = nullptr;  ///< 拥有
    QLocalSocket       m_socket;
    QByteArray         m_rx_buf;
    bool               m_hello_done = false;
};

#endif // BPLC_PLUGIN_HOST_H
