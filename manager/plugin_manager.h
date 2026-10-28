/// @file plugin_manager.h
/// @brief PluginManager:主程序侧插件管理(扫描/启动/监控/IPC)
/// @details 职责:
///   - 扫描 plugins/ 目录,读取 plugin.json
///   - 为每个插件创建 QLocalServer,启动 bplc-plugin-host 子进程
///   - 握手校验(api_version/schema),注册协议到工厂
///   - 同步 IPC 解析(worker 线程调用,阻塞等待)
///   - 心跳 + 崩溃检测(进程退出/心跳超时 → 禁用插件并通知)
///   - 单读者邮箱:读 socket 一律只在主线程 readyRead 槽里做,响应按
///     correlation_id 投进 mailbox;worker 等 QWaitCondition,主线程
///     等本地事件循环。旧"阻塞读 + readyRead 槽抢同一 socket"导致
///     ParseResponse 被槽吞掉而假超时的竞态已消除(2026-10-21 定案)。
///     写可由调用线程直接发(与旧行为一致,读写的竞态只在读一侧)。
#ifndef BPLC_PLUGIN_MANAGER_H
#define BPLC_PLUGIN_MANAGER_H

#include <QObject>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <QTimer>
#include <QWaitCondition>
#include <functional>

#include "bplcframe.h"
#include "igraphicsplugin.h"
#include "iprotocolparser.h"
#include "plugin_manifest.h"
#include "plugin_ipc.pb.h"

class QLocalServer;
class QLocalSocket;
class QProcess;
class PluginGraphicsView;

/// @brief 解析器注册回调(由主程序注入,解耦对具体协议工厂的依赖)
/// @details register_fn: protocol_id → 工厂函数; unregister_fn: 移除 protocol_id
struct ParserRegistry {
    std::function<void(const QString&,
                       std::function<std::unique_ptr<IProtocolParser>()>)> register_fn;
    std::function<void(const QString&)> unregister_fn;
};

/// @brief 单个插件的运行时状态
struct PluginRuntime {
    PluginManifest manifest;
    QProcess*     process = nullptr;
    QLocalServer* server = nullptr;
    QLocalSocket* socket = nullptr;   ///< 握手成功后的连接
    QByteArray    rx_buf;
    QMutex        ipc_mutex;          ///< 串行化该插件的 IPC 请求(每插件同时仅一等待者)
    quint64       seq = 0;
    bool          ready = false;      ///< 握手完成
    bool          disabled = false;   ///< 崩溃/错误后禁用
    QString       disable_reason;
    qint64        last_pong_ms = 0;
    // ---- 单读者邮箱(读只在主线程槽里发生) ----
    QMutex        mailbox_mutex;      ///< 保护 mailbox/pending_seqs/socket_gone
    QWaitCondition mailbox_cond;      ///< worker 等待者用
    QMap<quint64, qtplugin::ipc::Envelope> mailbox;  ///< 待取响应,按 correlation_id
    QSet<quint64> pending_seqs;       ///< 已登记等待的 seq(槽只投递这些)
    bool          socket_gone = false;///< 断开/禁用,等待者快速失败
};

/// @brief 插件管理器(单例,主线程)
class PluginManager : public QObject {
    Q_OBJECT
public:
    static PluginManager& instance();

    /// @brief 扫描并启动 plugins_dir 下所有插件
    void loadAll(const QString& plugins_dir);
    /// @brief 设置解析器注册回调(主程序在 loadAll 前调用,注入协议工厂)
    void set_parser_registry(ParserRegistry reg) { m_registry = std::move(reg); }
    /// @brief 已注册的插件协议 id 列表(供 UI/配置)
    QStringList pluginProtocolIds() const;
    /// @brief 协议 id 是否来自插件
    bool is_plugin_protocol(const QString& protocol_id) const;

    /// @brief 经插件解析一帧(阻塞,worker 线程调用)
    /// @return ok=false 时 result.accept=false 且 reject_reason 置错误信息
    ///         (超时/断开时返回 false 且不碰 *result,与旧行为一致)
    bool parse_via_plugin(const QString& protocol_id,
                        const BplcFrame& frame, MsduState& msdu,
                        const ParseFilter& filter, ParseResult* result);

    // ---- 图形(Phase3) ----
    /// @brief 插件是否有图形能力
    bool has_graphics(const QString& protocol_id) const;
    /// @brief 请求渲染(阻塞,主线程调用)。返回空图=失败
    QImage request_render(const QString& protocol_id, int w, int h);
    /// @brief 发送图形事件(阻塞,主线程调用)。返回 needs_redraw
    bool send_graphics_event(const QString& protocol_id,
                             const GraphicsEvent& e);
    /// @brief 注册图形视图(收 RequestRedraw 时通知重绘)
    void register_graphics_view(const QString& protocol_id,
                                PluginGraphicsView* view);
    void unregister_graphics_view(const QString& protocol_id,
                                  PluginGraphicsView* view);

signals:
    /// @brief 插件状态变化(崩溃/禁用/就绪),UI 可据此提示
    void plugin_status_changed(const QString& plugin_id, const QString& status);
    /// @brief mailbox 投递(主线程等待者的本地事件循环据此退出)
    void response_arrived(quint64 seq);

private:
    explicit PluginManager(QObject* parent = nullptr);
    ~PluginManager() override;

    bool start_plugin(const PluginManifest& m);
    void on_new_connection(const QString& plugin_id);
    void on_host_ready_read(const QString& plugin_id);
    void on_process_finished(const QString& plugin_id, int code);
    void on_process_error(const QString& plugin_id);
    void disable_plugin(const QString& plugin_id, const QString& reason);
    void handle_envelope(const QString& plugin_id,
                         const qtplugin::ipc::Envelope& env);
    void send_envelope(PluginRuntime* rt, const qtplugin::ipc::Envelope& env);
    /// @brief 响应投递:仅当 seq 已登记等待才进 mailbox(否则丢弃过期响应)
    void deliver_or_drop(PluginRuntime* rt, const qtplugin::ipc::Envelope& env);
    void register_pending(PluginRuntime* rt, quint64 seq);
    void unregister_pending(PluginRuntime* rt, quint64 seq);
    /// @brief worker 线程等待(不碰 socket,只等 mailbox)
    bool wait_for_envelope_worker(PluginRuntime* rt, quint64 seq,
                                  int timeout_ms,
                                  qtplugin::ipc::Envelope* out);
    /// @brief 主线程等待(本地事件循环泵消息,让 readyRead 槽能跑)
    bool wait_for_envelope_main(PluginRuntime* rt, quint64 seq,
                                int timeout_ms,
                                qtplugin::ipc::Envelope* out);
    void checkHeartbeats();

    QMap<QString, PluginRuntime*> m_plugins;  ///< key: protocol_id
    QMap<QString, QList<PluginGraphicsView*>> m_graphics_views;  ///< key: protocol_id
    QTimer m_hb_timer;
    ParserRegistry m_registry;  ///< 主程序注入的解析器注册回调
};

#endif // BPLC_PLUGIN_MANAGER_H
