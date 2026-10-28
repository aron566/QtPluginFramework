/// @file plugin_manager.cpp
/// @brief PluginManager 实现(protobuf Envelope + 单读者邮箱版)
/// @details 读 socket 一律只在主线程 on_host_ready_read 里做,响应按
///          correlation_id 投进 mailbox;同步等待者按线程分流:
///          worker 线程等 QWaitCondition,主线程跑本地事件循环。
///          旧"阻塞读 + readyRead 槽抢同一 socket"导致 ParseResponse
///          被槽吞掉而假超时的竞态已消除(2026-10-21 定案)。
#include "plugin_manager.h"
#include "plugin_graphics_view.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutexLocker>
#include <QProcess>
#include <QThread>
#include <QTimer>

#include "plugin_ipc.h"
#include "plugin_serialization.h"
#include "plugin_parser_proxy.h"

namespace {
constexpr int kParseTimeoutMs = 5000;
constexpr int kGraphicsTimeoutMs = 5000;
constexpr int kHeartbeatIntervalMs = 5000;
constexpr int kHeartbeatTimeoutMs = 15000;
} // namespace

PluginManager& PluginManager::instance() {
    static PluginManager inst;
    return inst;
}

PluginManager::PluginManager(QObject* parent) : QObject(parent) {
    m_hb_timer.setInterval(kHeartbeatIntervalMs);
    connect(&m_hb_timer, &QTimer::timeout, this, &PluginManager::checkHeartbeats);
    m_hb_timer.start();
}

PluginManager::~PluginManager() {
    for (auto* rt : m_plugins) {
        if (rt->socket) {
            // 优雅退出
            qtplugin::ipc::Envelope env;
            env.set_shutdown(true);
            send_envelope(rt, env);
            rt->socket->flush();
        }
        if (rt->process) {
            rt->process->terminate();
            rt->process->waitForFinished(2000);
            delete rt->process;
        }
        delete rt->server;
        delete rt;
    }
}

void PluginManager::loadAll(const QString& plugins_dir) {
    const QDir d(plugins_dir);
    if (!d.exists()) return;
    for (const QString& sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const PluginManifest m = read_plugin_manifest(d.filePath(sub));
        if (!m.valid) continue;  // 无效清单跳过(可日志)
        if (m.runtime != QStringLiteral("native")
            && m.runtime != QStringLiteral("js")
            && m.runtime != QStringLiteral("lua")) continue;  // Phase2: native/js/lua
        if (m_plugins.contains(m.protocol_id)) continue;      // 重复协议 id
        start_plugin(m);
    }
}

QStringList PluginManager::pluginProtocolIds() const {
    return m_plugins.keys();
}

bool PluginManager::is_plugin_protocol(const QString& protocol_id) const {
    auto it = m_plugins.find(protocol_id);
    return it != m_plugins.end() && (*it)->ready && !(*it)->disabled;
}

static QString hostBinaryPath() {
    // bplc-plugin-host 与主程序同目录
    return QCoreApplication::applicationDirPath()
         + QStringLiteral("/bplc-plugin-host");
}

bool PluginManager::start_plugin(const PluginManifest& m) {
    auto* rt = new PluginRuntime;
    rt->manifest = m;
    const QString sock_name = plugin_ipc::socketName(m.name);
    QLocalServer::removeServer(sock_name);

    rt->server = new QLocalServer(this);
    if (!rt->server->listen(sock_name)) {
        delete rt;
        return false;
    }
    const QString pid = m.protocol_id;
    connect(rt->server, &QLocalServer::newConnection,
            this, [this, pid]() { on_new_connection(pid); });

    rt->process = new QProcess(this);
    connect(rt->process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, pid](int code, QProcess::ExitStatus) {
                on_process_finished(pid, code);
            });
    connect(rt->process, &QProcess::errorOccurred,
            this, [this, pid]() { on_process_error(pid); });

    rt->process->start(hostBinaryPath(),
        {QStringLiteral("--socket"), sock_name,
         QStringLiteral("--plugin"), m.dir_path});
    if (!rt->process->waitForStarted(5000)) {
        delete rt;
        return false;
    }
    m_plugins.insert(pid, rt);
    return true;
}

void PluginManager::on_new_connection(const QString& plugin_id) {
    auto it = m_plugins.find(plugin_id);
    if (it == m_plugins.end()) return;
    PluginRuntime* rt = *it;
    rt->socket = rt->server->nextPendingConnection();
    connect(rt->socket, &QLocalSocket::readyRead,
            this, [this, plugin_id]() { on_host_ready_read(plugin_id); });
    connect(rt->socket, &QLocalSocket::disconnected,
            this, [this, plugin_id, rt]() {
                // 等待者快速失败(不再等到超时)
                {
                    QMutexLocker lk(&rt->mailbox_mutex);
                    rt->socket_gone = true;
                }
                rt->mailbox_cond.wakeAll();
                emit response_arrived(0);  // 唤醒主线程等待者(其后查 socket_gone)
                disable_plugin(plugin_id, QStringLiteral("host disconnected"));
            });
}

void PluginManager::on_host_ready_read(const QString& plugin_id) {
    auto it = m_plugins.find(plugin_id);
    if (it == m_plugins.end() || !(*it)->socket) return;
    PluginRuntime* rt = *it;
    rt->rx_buf.append(rt->socket->readAll());
    // 帧:[quint32 大端长度][Envelope protobuf 字节]
    while (rt->rx_buf.size() >= 4) {
        const quint32 len =
            (static_cast<quint8>(rt->rx_buf[0]) << 24) |
            (static_cast<quint8>(rt->rx_buf[1]) << 16) |
            (static_cast<quint8>(rt->rx_buf[2]) << 8) |
            static_cast<quint8>(rt->rx_buf[3]);
        if (rt->rx_buf.size() < 4 + static_cast<int>(len)) break;  // 等更多数据
        const QByteArray bytes = rt->rx_buf.mid(4, len);
        rt->rx_buf.remove(0, 4 + len);
        qtplugin::ipc::Envelope env;
        if (!env.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())))
            continue;  // 坏包丢弃,长度成帧保证不失同步
        handle_envelope(plugin_id, env);
    }
}

void PluginManager::handle_envelope(const QString& plugin_id,
                                    const qtplugin::ipc::Envelope& env) {
    auto it = m_plugins.find(plugin_id);
    if (it == m_plugins.end()) return;
    PluginRuntime* rt = *it;
    using Body = qtplugin::ipc::Envelope::BodyCase;
    switch (env.body_case()) {
    case Body::kHello: {
        const qtplugin::ipc::Hello& h = env.hello();
        const bool ok = (static_cast<int>(h.api_version()) == plugin_ipc::kApiVersion)
                     && (QString::fromStdString(h.plugin_id()) == rt->manifest.protocol_id)
                     && (static_cast<int>(h.proto_schema_version())
                         == plugin_ipc::kProtoSchemaVersion);
        qtplugin::ipc::Envelope ack;
        ack.set_correlation_id(env.correlation_id());
        qtplugin::ipc::HelloAck* a = ack.mutable_hello_ack();
        a->set_ok(ok);
        a->set_reason(ok ? std::string()
                         : "api_version/protocol/schema mismatch");
        a->set_negotiated_schema_version(plugin_ipc::kProtoSchemaVersion);
        send_envelope(rt, ack);
        if (ok) {
            rt->ready = true;
            rt->last_pong_ms = QDateTime::currentMSecsSinceEpoch();
            // 注册到解析器工厂(插件协议 id → 代理),经主程序注入的回调
            const QString pid = rt->manifest.protocol_id;
            if (m_registry.register_fn)
                m_registry.register_fn(pid, [pid]() { return make_plugin_parser(pid); });
            emit plugin_status_changed(plugin_id, QStringLiteral("ready"));
        } else {
            disable_plugin(plugin_id, QStringLiteral("handshake failed"));
        }
        break;
    }
    case Body::kPong:
        rt->last_pong_ms = QDateTime::currentMSecsSinceEpoch();
        deliver_or_drop(rt, env);  // 心跳 Ping 不登记等待,此处仅更新心跳
        break;
    case Body::kErrorMsg:
        // 解析错误不禁用插件,仅记录(可日志)
        break;
    case Body::kRequestRedraw:
        // 插件主动请求重绘:通知所有注册的视图
        for (PluginGraphicsView* v : m_graphics_views.value(plugin_id))
            v->on_plugin_request_redraw();
        break;
    case Body::kParseResponse:
    case Body::kRenderResponse:
    case Body::kEventAck:
        deliver_or_drop(rt, env);
        break;
    default:
        break;  // 未知/单向消息忽略
    }
}

void PluginManager::deliver_or_drop(PluginRuntime* rt,
                                    const qtplugin::ipc::Envelope& env) {
    const quint64 seq = env.correlation_id();
    bool deliver = false;
    {
        QMutexLocker lk(&rt->mailbox_mutex);
        deliver = rt->pending_seqs.contains(seq);
        if (deliver) rt->mailbox.insert(seq, env);
    }
    if (deliver) {
        rt->mailbox_cond.wakeAll();
        emit response_arrived(seq);
    }
    // 未登记等待的响应(过期/心跳)直接丢弃,不进 mailbox
}

void PluginManager::register_pending(PluginRuntime* rt, quint64 seq) {
    QMutexLocker lk(&rt->mailbox_mutex);
    rt->pending_seqs.insert(seq);
}

void PluginManager::unregister_pending(PluginRuntime* rt, quint64 seq) {
    QMutexLocker lk(&rt->mailbox_mutex);
    rt->pending_seqs.remove(seq);
    rt->mailbox.remove(seq);
}

bool PluginManager::wait_for_envelope_worker(PluginRuntime* rt, quint64 seq,
                                            int timeout_ms,
                                            qtplugin::ipc::Envelope* out) {
    QMutexLocker lk(&rt->mailbox_mutex);
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeout_ms;
    while (true) {
        auto it = rt->mailbox.find(seq);
        if (it != rt->mailbox.end()) {
            *out = it.value();
            rt->mailbox.erase(it);
            return true;
        }
        if (rt->socket_gone) return false;
        const qint64 remain = deadline - QDateTime::currentMSecsSinceEpoch();
        if (remain <= 0) return false;
        rt->mailbox_cond.wait(&rt->mailbox_mutex,
                              static_cast<unsigned long>(remain));
    }
}

bool PluginManager::wait_for_envelope_main(PluginRuntime* rt, quint64 seq,
                                           int timeout_ms,
                                           qtplugin::ipc::Envelope* out) {
    // 响应可能在等待开始前已到,先查一次
    {
        QMutexLocker lk(&rt->mailbox_mutex);
        auto it = rt->mailbox.find(seq);
        if (it != rt->mailbox.end()) {
            *out = it.value();
            rt->mailbox.erase(it);
            return true;
        }
        if (rt->socket_gone) return false;
    }
    QEventLoop loop;
    QTimer deadline_timer;
    deadline_timer.setSingleShot(true);
    connect(&deadline_timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    // 投递信号:槽投 mailbox 后发,等待者退出本地循环
    connect(this, &PluginManager::response_arrived, &loop,
            [&loop](quint64 s) {
                Q_UNUSED(s);
                loop.quit();
            });
    // 兜底轮询:投递与 connect 之间的竞态窗口(信号已发但无人接)由它补
    QTimer ticker;
    ticker.setInterval(100);
    connect(&ticker, &QTimer::timeout, &loop, [&loop, rt, seq]() {
        QMutexLocker lk(&rt->mailbox_mutex);
        if (rt->mailbox.contains(seq) || rt->socket_gone) loop.quit();
    });
    ticker.start();
    deadline_timer.start(timeout_ms);
    loop.exec();
    QMutexLocker lk(&rt->mailbox_mutex);
    auto it = rt->mailbox.find(seq);
    if (it != rt->mailbox.end()) {
        *out = it.value();
        rt->mailbox.erase(it);
        return true;
    }
    return false;
}

void PluginManager::send_envelope(PluginRuntime* rt,
                                  const qtplugin::ipc::Envelope& env) {
    if (!rt->socket) return;
    std::string bytes;
    if (!env.SerializeToString(&bytes)) return;
    QByteArray frame;
    const quint32 len = static_cast<quint32>(bytes.size());
    frame.append(static_cast<char>((len >> 24) & 0xFF));
    frame.append(static_cast<char>((len >> 16) & 0xFF));
    frame.append(static_cast<char>((len >> 8) & 0xFF));
    frame.append(static_cast<char>(len & 0xFF));
    frame.append(bytes.data(), static_cast<int>(bytes.size()));
    rt->socket->write(frame);
    rt->socket->flush();
}

bool PluginManager::parse_via_plugin(const QString& protocol_id,
                                   const BplcFrame& frame, MsduState& msdu,
                                   const ParseFilter& filter,
                                   ParseResult* result) {
    auto it = m_plugins.find(protocol_id);
    if (it == m_plugins.end()) return false;
    PluginRuntime* rt = *it;
    if (!rt->ready || rt->disabled || !rt->socket) return false;

    QMutexLocker lock(&rt->ipc_mutex);
    const quint64 seq = ++rt->seq;
    register_pending(rt, seq);

    qtplugin::ipc::Envelope env;
    env.set_correlation_id(seq);
    qtplugin::ipc::ParseRequest* req = env.mutable_parse_request();
    *req->mutable_frame() = plugin_proto::to_proto(frame);
    *req->mutable_msdu() = plugin_proto::to_proto(msdu);
    *req->mutable_filter() = plugin_proto::to_proto(filter);
    send_envelope(rt, env);

    // 阻塞等待响应(worker 线程调用,不阻塞 UI;读只在主线程槽里做)
    qtplugin::ipc::Envelope resp;
    const bool got = wait_for_envelope_worker(rt, seq, kParseTimeoutMs, &resp);
    unregister_pending(rt, seq);
    if (!got
        || resp.body_case() != qtplugin::ipc::Envelope::kParseResponse) {
        // 超时/断开:很可能 host 挂了,禁用插件(与旧行为一致:不碰 *result)
        // 注意:不能在 worker 线程直接调 disable_plugin(涉及信号),标记即可
        rt->disabled = true;
        rt->disable_reason = QStringLiteral("plugin parse timeout/crash");
        QMetaObject::invokeMethod(this, [this, protocol_id]() {
            emit plugin_status_changed(protocol_id, QStringLiteral("timeout-disabled"));
        }, Qt::QueuedConnection);
        return false;
    }
    const qtplugin::ipc::ParseResponse& pr = resp.parse_response();
    *result = plugin_proto::from_proto(pr.result());
    msdu = plugin_proto::from_proto(pr.msdu());
    // 双写:外层 accepted 为准(与旧线格式 ok 字段语义一致;host 侧两者同值)
    result->accept = pr.accepted();
    return true;
}

void PluginManager::on_process_finished(const QString& plugin_id, int code) {
    Q_UNUSED(code);
    auto it = m_plugins.find(plugin_id);
    if (it == m_plugins.end()) return;
    PluginRuntime* rt = *it;
    if (!rt->disabled) {
        // 非预期退出(未主动禁用):视为崩溃
        disable_plugin(plugin_id, QStringLiteral("host process exited unexpectedly"));
    }
}

void PluginManager::on_process_error(const QString& plugin_id) {
    disable_plugin(plugin_id, QStringLiteral("host process error"));
}

void PluginManager::disable_plugin(const QString& plugin_id,
                                  const QString& reason) {
    auto it = m_plugins.find(plugin_id);
    if (it == m_plugins.end() || (*it)->disabled) return;
    PluginRuntime* rt = *it;
    rt->disabled = true;
    rt->disable_reason = reason;
    rt->ready = false;
    {
        QMutexLocker lk(&rt->mailbox_mutex);
        rt->socket_gone = true;
    }
    rt->mailbox_cond.wakeAll();
    emit response_arrived(0);
    if (m_registry.unregister_fn)
        m_registry.unregister_fn(rt->manifest.protocol_id);
    emit plugin_status_changed(plugin_id, QStringLiteral("disabled: ") + reason);
}

void PluginManager::checkHeartbeats() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_plugins.begin(); it != m_plugins.end(); ++it) {
        PluginRuntime* rt = it.value();
        if (!rt->ready || rt->disabled || !rt->socket) continue;
        // 发 Ping(不登记等待;Pong 回来只更新心跳,见 handle_envelope)
        {
            QMutexLocker lock(&rt->ipc_mutex);
            qtplugin::ipc::Envelope env;
            env.set_correlation_id(++rt->seq);
            env.set_ping(true);
            send_envelope(rt, env);
        }
        if (now - rt->last_pong_ms > kHeartbeatTimeoutMs) {
            disable_plugin(it.key(), QStringLiteral("heartbeat timeout"));
        }
    }
}

// ---- 图形(Phase3) ----

bool PluginManager::has_graphics(const QString& protocol_id) const {
    auto it = m_plugins.find(protocol_id);
    if (it == m_plugins.end()) return false;
    return (*it)->manifest.graphics;
}

QImage PluginManager::request_render(const QString& protocol_id,
                                     int w, int h) {
    auto it = m_plugins.find(protocol_id);
    if (it == m_plugins.end()) return QImage();
    PluginRuntime* rt = *it;
    if (!rt->ready || rt->disabled || !rt->socket) return QImage();
    if (!rt->manifest.graphics) return QImage();

    QMutexLocker lock(&rt->ipc_mutex);
    const quint64 seq = ++rt->seq;
    register_pending(rt, seq);
    qtplugin::ipc::Envelope env;
    env.set_correlation_id(seq);
    qtplugin::ipc::RenderRequest* req = env.mutable_render_request();
    req->set_width(w);
    req->set_height(h);
    send_envelope(rt, env);

    qtplugin::ipc::Envelope resp;
    const bool got = wait_for_envelope_main(rt, seq, kGraphicsTimeoutMs, &resp);
    unregister_pending(rt, seq);
    if (!got
        || resp.body_case() != qtplugin::ipc::Envelope::kRenderResponse)
        return QImage();
    const qtplugin::ipc::RenderResponse& rr = resp.render_response();
    if (!rr.ok()) return QImage();
    return plugin_proto::image_from_png(
        QByteArray(rr.png_image().data(),
                   static_cast<int>(rr.png_image().size())));
}

bool PluginManager::send_graphics_event(const QString& protocol_id,
                                        const GraphicsEvent& e) {
    auto it = m_plugins.find(protocol_id);
    if (it == m_plugins.end()) return false;
    PluginRuntime* rt = *it;
    if (!rt->ready || rt->disabled || !rt->socket) return false;
    if (!rt->manifest.graphics) return false;

    QMutexLocker lock(&rt->ipc_mutex);
    const quint64 seq = ++rt->seq;
    register_pending(rt, seq);
    qtplugin::ipc::Envelope env;
    env.set_correlation_id(seq);
    *env.mutable_graphics_event()->mutable_event() = plugin_proto::to_proto(e);
    send_envelope(rt, env);

    qtplugin::ipc::Envelope resp;
    const bool got = wait_for_envelope_main(rt, seq, kGraphicsTimeoutMs, &resp);
    unregister_pending(rt, seq);
    if (!got || resp.body_case() != qtplugin::ipc::Envelope::kEventAck)
        return false;
    return resp.event_ack().needs_redraw();
}

void PluginManager::register_graphics_view(const QString& protocol_id,
                                           PluginGraphicsView* view) {
    m_graphics_views[protocol_id].append(view);
    connect(view, &QObject::destroyed, this,
            [this, protocol_id, view]() { unregister_graphics_view(protocol_id, view); });
}

void PluginManager::unregister_graphics_view(const QString& protocol_id,
                                             PluginGraphicsView* view) {
    m_graphics_views[protocol_id].removeAll(view);
}
