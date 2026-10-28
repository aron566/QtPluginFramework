/// @file plugin_host.cpp
/// @brief bplc-plugin-host 实现(多 backend,protobuf Envelope 版)
#include "plugin_host.h"

#include <QCoreApplication>
#include <QTimer>

#include "plugin_backend.h"
#include "plugin_serialization.h"

PluginHost::PluginHost(const QString& socket_name, const QString& plugin_dir,
                       QObject* parent)
    : QObject(parent), m_socket_name(socket_name) {
    m_manifest = read_plugin_manifest(plugin_dir);
}

PluginHost::~PluginHost() {
    delete m_backend;
    m_backend = nullptr;
}

bool PluginHost::start(QString* err) {
    if (!m_manifest.valid) {
        *err = QStringLiteral("bad manifest: %1").arg(m_manifest.error);
        return false;
    }
    if (m_manifest.api_version != plugin_ipc::kApiVersion) {
        *err = QStringLiteral("api_version mismatch: plugin=%1 host=%2")
                   .arg(m_manifest.api_version).arg(plugin_ipc::kApiVersion);
        return false;
    }

    m_backend = create_backend(m_manifest.runtime);
    if (!m_backend) {
        *err = QStringLiteral("unsupported runtime '%1'")
                   .arg(m_manifest.runtime);
        return false;
    }
    if (!m_backend->initialize(m_manifest, err)) {
        delete m_backend;
        m_backend = nullptr;
        return false;
    }
    // 插件主动请求重绘 → 发 RequestRedraw 给主进程(单向,correlation_id=0)
    PluginHost* self = this;
    m_backend->set_redraw_callback([self]() {
        qtplugin::ipc::Envelope env;
        env.set_request_redraw(true);
        self->send_envelope(env);
    });
    // 后端协议 id 须与清单一致
    if (m_backend->protocol_id() != m_manifest.protocol_id) {
        *err = QStringLiteral("protocolId mismatch: manifest=%1 backend=%2")
                   .arg(m_manifest.protocol_id, m_backend->protocol_id());
        delete m_backend;
        m_backend = nullptr;
        return false;
    }

    connect(&m_socket, &QLocalSocket::connected, this, &PluginHost::on_connected);
    connect(&m_socket, &QLocalSocket::readyRead, this, &PluginHost::on_ready_read);
    connect(&m_socket, &QLocalSocket::disconnected,
            qApp, &QCoreApplication::quit);
    connect(&m_socket, &QLocalSocket::errorOccurred,
            this, &PluginHost::on_socket_error);
    m_socket.connectToServer(m_socket_name);
    if (!m_socket.waitForConnected(10000)) {
        *err = QStringLiteral("connect to main app failed: %1")
                   .arg(m_socket.errorString());
        return false;
    }
    return true;
}

int PluginHost::exec() {
    return QCoreApplication::exec();
}

void PluginHost::on_connected() {
    // 握手:Hello(能力 + schema 协商;correlation_id=0,HelloAck 同值回)
    qtplugin::ipc::Envelope env;
    qtplugin::ipc::Hello* h = env.mutable_hello();
    h->set_api_version(plugin_ipc::kApiVersion);
    h->set_plugin_name(m_manifest.name.toStdString());
    h->set_plugin_id(m_manifest.protocol_id.toStdString());
    if (m_backend->has_graphics())
        h->add_capabilities("graphics");
    h->add_capabilities("parse");
    h->set_proto_schema_version(plugin_ipc::kProtoSchemaVersion);
    send_envelope(env);
}

void PluginHost::on_ready_read() {
    m_rx_buf.append(m_socket.readAll());
    // 帧:[quint32 len][Envelope protobuf 字节]
    while (m_rx_buf.size() >= 4) {
        const quint32 len =
            (static_cast<quint8>(m_rx_buf[0]) << 24) |
            (static_cast<quint8>(m_rx_buf[1]) << 16) |
            (static_cast<quint8>(m_rx_buf[2]) << 8) |
            static_cast<quint8>(m_rx_buf[3]);
        if (m_rx_buf.size() < 4 + static_cast<int>(len)) break;  // 等更多数据
        const QByteArray bytes = m_rx_buf.mid(4, len);
        m_rx_buf.remove(0, 4 + len);

        qtplugin::ipc::Envelope env;
        if (!env.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())))
            continue;  // 坏包丢弃,长度成帧保证不失同步
        handle_envelope(env);
    }
}

void PluginHost::on_disconnected() {
    QCoreApplication::quit();  // 主程序断开 → 退出
}

void PluginHost::on_socket_error(QLocalSocket::LocalSocketError e) {
    if (e != QLocalSocket::PeerClosedError)
        QCoreApplication::exit(2);
}

void PluginHost::send_envelope(const qtplugin::ipc::Envelope& env) {
    std::string bytes;
    if (!env.SerializeToString(&bytes)) return;
    QByteArray frame;
    const quint32 len = static_cast<quint32>(bytes.size());
    frame.append(static_cast<char>((len >> 24) & 0xFF));
    frame.append(static_cast<char>((len >> 16) & 0xFF));
    frame.append(static_cast<char>((len >> 8) & 0xFF));
    frame.append(static_cast<char>(len & 0xFF));
    frame.append(bytes.data(), static_cast<int>(bytes.size()));
    m_socket.write(frame);
    m_socket.flush();
}

void PluginHost::handle_envelope(const qtplugin::ipc::Envelope& env) {
    switch (env.body_case()) {
    case qtplugin::ipc::Envelope::kHelloAck: {
        m_hello_done = env.hello_ack().ok();
        if (!m_hello_done) QCoreApplication::exit(3);
        break;
    }
    case qtplugin::ipc::Envelope::kParseRequest:
        handle_parse_request(env.correlation_id(), env.parse_request());
        break;
    case qtplugin::ipc::Envelope::kParseBatchRequest:
        handle_parse_batch(env.correlation_id(), env.parse_batch_request());
        break;
    case qtplugin::ipc::Envelope::kRenderRequest:
        handle_render_request(env.correlation_id(), env.render_request());
        break;
    case qtplugin::ipc::Envelope::kGraphicsEvent:
        handle_graphics_event(env.correlation_id(), env.graphics_event());
        break;
    case qtplugin::ipc::Envelope::kPing: {
        qtplugin::ipc::Envelope resp;
        resp.set_correlation_id(env.correlation_id());
        resp.set_pong(true);
        send_envelope(resp);
        break;
    }
    case qtplugin::ipc::Envelope::kShutdown:
        QCoreApplication::quit();
        break;
    default:
        break;  // 未知/单向消息忽略
    }
}

qtplugin::ipc::ParseResponse PluginHost::do_parse(
    const qtplugin::ipc::ParseRequest& req) {
    qtplugin::ipc::ParseResponse resp;
    if (!m_hello_done || !m_backend) {
        // 旧线格式的 ok=false + "not ready" 对应:accepted=false,
        // result.accept=false 且 reject_reason 说明原因
        resp.set_accepted(false);
        resp.mutable_result()->set_accept(false);
        resp.mutable_result()->set_reject_reason("not ready");
        *resp.mutable_msdu() = req.msdu();
        return resp;
    }
    BplcFrame frame = plugin_proto::from_proto(req.frame());
    MsduState msdu = plugin_proto::from_proto(req.msdu());
    ParseFilter filter = plugin_proto::from_proto(req.filter());
    QString err;
    const ParseResult r = m_backend->parse(frame, msdu, filter, &err);
    // JS 等脚本后端可能置 err:accept=false 时 reject_reason 已填
    Q_UNUSED(err);
    resp.set_accepted(r.accept);
    *resp.mutable_result() = plugin_proto::to_proto(r);
    *resp.mutable_msdu() = plugin_proto::to_proto(msdu);
    return resp;
}

void PluginHost::handle_parse_request(quint64 corr,
                                      const qtplugin::ipc::ParseRequest& req) {
    qtplugin::ipc::Envelope env;
    env.set_correlation_id(corr);
    *env.mutable_parse_response() = do_parse(req);
    send_envelope(env);
}

void PluginHost::handle_parse_batch(quint64 corr,
                                    const qtplugin::ipc::ParseBatchRequest& batch) {
    // 批内按顺序解析,MsduState 链式传递:首条用其自带 msdu,后续条目
    // 用前一条的输出 msdu(自带 msdu 被忽略);响应按 item_seq 原样回填。
    qtplugin::ipc::Envelope env;
    env.set_correlation_id(corr);
    qtplugin::ipc::ParseBatchResponse* out = env.mutable_parse_batch_response();
    qtplugin::ipc::ParseRequest chained;
    for (int i = 0; i < batch.items_size(); ++i) {
        const qtplugin::ipc::ParseBatchItem& item = batch.items(i);
        if (i == 0) {
            chained = item.request();
        } else {
            chained = item.request();
            *chained.mutable_msdu() = out->items(i - 1).response().msdu();
        }
        qtplugin::ipc::ParseBatchItemResponse* r = out->add_items();
        r->set_item_seq(item.item_seq());
        *r->mutable_response() = do_parse(chained);
    }
    send_envelope(env);
}

void PluginHost::handle_render_request(quint64 corr,
                                       const qtplugin::ipc::RenderRequest& req) {
    qtplugin::ipc::Envelope env;
    env.set_correlation_id(corr);
    qtplugin::ipc::RenderResponse* resp = env.mutable_render_response();
    if (!m_hello_done || !m_backend || !m_backend->has_graphics()) {
        resp->set_ok(false);
    } else {
        QString err;
        const QImage img = m_backend->render_graphics(req.width(), req.height(), &err);
        Q_UNUSED(err);
        resp->set_ok(!img.isNull());
        if (!img.isNull())
            resp->set_png_image(plugin_proto::image_to_png(img).toStdString());
    }
    send_envelope(env);
}

void PluginHost::handle_graphics_event(quint64 corr,
                                       const qtplugin::ipc::GraphicsEventMsg& msg) {
    qtplugin::ipc::Envelope env;
    env.set_correlation_id(corr);
    qtplugin::ipc::EventAck* ack = env.mutable_event_ack();
    if (!m_hello_done || !m_backend || !m_backend->has_graphics()) {
        ack->set_needs_redraw(false);
    } else {
        QString err;
        ack->set_needs_redraw(
            m_backend->handle_graphics_event(
                plugin_proto::from_proto(msg.event()), &err));
        Q_UNUSED(err);
    }
    send_envelope(env);
}
