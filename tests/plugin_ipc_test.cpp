/// @file plugin_ipc_test.cpp
/// @brief 插件 IPC 集成测试:启动 bplc-plugin-host,验证 3 种新插件
/// @details 用法: plugin_ipc_test --host <path-to-bplc-plugin-host> --examples <dir>
///          测试 js_topo/lua_diag/lua_report 的解析、图形渲染、事件、
///          心跳 Ping/Pong 与批解析。线格式为 protobuf Envelope 版
///          (见 ipc/plugin_ipc.h):[quint32 大端长度][Envelope 字节]。
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QThread>
#include <cstdio>
#include <QTextStream>
#include <QDir>

#include "../ipc/plugin_ipc.h"
#include "../ipc/plugin_ipc.pb.h"
#include "../ipc/plugin_serialization.h"

namespace {

QTextStream& log() {
    static QTextStream s(stdout);
    return s;
}

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const QString& name, const QString& detail = {}) {
    if (ok) {
        ++g_pass;
        log() << "  [PASS] " << name;
        if (!detail.isEmpty()) log() << " - " << detail;
        log() << "\n";
    } else {
        ++g_fail;
        log() << "  [FAIL] " << name;
        if (!detail.isEmpty()) log() << " - " << detail;
        log() << "\n";
    }
}

void send_envelope(QLocalSocket* sock, const qtplugin::ipc::Envelope& env) {
    std::string bytes;
    if (!env.SerializeToString(&bytes)) return;
    QByteArray frame;
    const quint32 len = static_cast<quint32>(bytes.size());
    frame.append(static_cast<char>((len >> 24) & 0xFF));
    frame.append(static_cast<char>((len >> 16) & 0xFF));
    frame.append(static_cast<char>((len >> 8) & 0xFF));
    frame.append(static_cast<char>(len & 0xFF));
    frame.append(bytes.data(), static_cast<int>(bytes.size()));
    sock->write(frame);
    sock->flush();
}

struct RecvResult {
    bool ok = false;
    qtplugin::ipc::Envelope env;
    QString error;
};

RecvResult recv_envelope(QLocalSocket* sock, int timeout_ms = 15000) {
    RecvResult r;
    fprintf(stdout, "DEBUG: recv_envelope waiting for header...\n");
    fflush(stdout);
    // 读 4 字节大端长度(先 drain 缓冲,再等新数据:见 2c71b1a 的教训,
    // QLocalSocket 在数据已到达时 waitForReadyRead 仍可能返回 false)
    QByteArray hdr;
    QElapsedTimer timer;
    timer.start();
    while (hdr.size() < 4) {
        if (sock->bytesAvailable() > 0) {
            hdr.append(sock->read(4 - hdr.size()));
            continue;
        }
        if (!sock->waitForReadyRead(100)) {
            if (timer.elapsed() > timeout_ms) {
                r.error = QStringLiteral("timeout waiting for header");
                return r;
            }
            continue;
        }
    }
    fprintf(stdout, "DEBUG: got header, len bytes\n");
    fflush(stdout);
    const quint32 len =
        (static_cast<quint8>(hdr[0]) << 24) |
        (static_cast<quint8>(hdr[1]) << 16) |
        (static_cast<quint8>(hdr[2]) << 8) |
        static_cast<quint8>(hdr[3]);
    if (len < 1 || len > 32 * 1024 * 1024) {
        r.error = QStringLiteral("bad frame len %1").arg(len);
        return r;
    }
    QByteArray body;
    while (body.size() < (int)len) {
        // 先 drain 已缓冲的数据:QLocalSocket 的 waitForReadyRead 在数据
        // 已到达(与 header 同包到达)时仍可能返回 false、只等新数据;
        // 不先读缓冲会恒超时(实测 avail=49 时 wait 恒 false)。
        if (sock->bytesAvailable() > 0) {
            body.append(sock->read(len - body.size()));
            continue;
        }
        if (!sock->waitForReadyRead(1000)) {
            if (timer.elapsed() > timeout_ms) {
                r.error = QStringLiteral("timeout waiting for body");
                return r;
            }
            continue;
        }
        body.append(sock->read(len - body.size()));
    }
    if (!r.env.ParseFromArray(body.constData(), static_cast<int>(body.size()))) {
        r.error = QStringLiteral("bad envelope bytes");
        return r;
    }
    r.ok = true;
    return r;
}

BplcFrame make_test_frame(const QByteArray& data, qint64 arrival_us) {
    BplcFrame f;
    f.data = data;
    f.arrival_us = arrival_us;
    return f;
}

/// @brief 测试单个插件,返回 true=全部通过
bool test_plugin(const QString& host_bin, const QString& plugin_dir,
                 const QString& plugin_name, bool test_graphics) {
    log() << "\n=== Testing " << plugin_name << " ===\n";

    const QString sock_name = QStringLiteral("test_%1_%2")
        .arg(plugin_name).arg(QCoreApplication::applicationPid());
    fprintf(stdout, "DEBUG: sock_name=%s\n", qPrintable(sock_name));
    fflush(stdout);
    QLocalServer::removeServer(sock_name);
    QLocalServer server;
    fprintf(stdout, "DEBUG: listening...\n");
    fflush(stdout);
    if (!server.listen(sock_name)) {
        check(false, QStringLiteral("server listen"), server.errorString());
        return false;
    }
    fprintf(stdout, "DEBUG: listening ok, fullName=%s\n",
            qPrintable(server.fullServerName()));
    fflush(stdout);

    QProcess host;
    fprintf(stdout, "DEBUG: starting host %s\n", qPrintable(host_bin));
    fflush(stdout);
    host.start(host_bin, {QStringLiteral("--socket"), sock_name,
                          QStringLiteral("--plugin"), plugin_dir});
    fprintf(stdout, "DEBUG: waitForStarted...\n");
    fflush(stdout);
    if (!host.waitForStarted(5000)) {
        check(false, QStringLiteral("host start"), host.errorString());
        return false;
    }
    fprintf(stdout, "DEBUG: host started, waiting for connection...\n");
    fflush(stdout);

    // 轮询等待连接(兼容 offscreen 模式)
    QLocalSocket* pending_sock = nullptr;
    for (int i = 0; i < 100; ++i) {
        // 让 server 处理待接受的连接
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (server.hasPendingConnections()) {
            pending_sock = server.nextPendingConnection();
            break;
        }
        // 检查 host 是否已退出
        if (host.state() == QProcess::NotRunning) {
            QString host_err = QString::fromLocal8Bit(host.readAllStandardError());
            fprintf(stdout, "DEBUG: host exited early. stderr: %s\n",
                    qPrintable(host_err));
            fflush(stdout);
            break;
        }
        QThread::msleep(100);
    }
    fprintf(stdout, "DEBUG: poll done, pending=%d\n",
            pending_sock ? 1 : 0);
    fflush(stdout);
    if (!pending_sock) {
        QString host_err = QString::fromLocal8Bit(host.readAllStandardError());
        fprintf(stdout, "DEBUG: no connection. host stderr: %s\n",
                qPrintable(host_err));
        fflush(stdout);
        check(false, QStringLiteral("host connect"),
              QStringLiteral("timeout; host stderr: %1").arg(host_err.left(200)));
        host.kill();
        return false;
    }
    QLocalSocket* sock = pending_sock;
    check(sock != nullptr, QStringLiteral("accept connection"));
    if (!sock) { host.kill(); return false; }

    bool all_ok = true;
    auto mark = [&](bool ok) { if (!ok) all_ok = false; };

    // 1. Hello (host→app)
    {
        RecvResult rr = recv_envelope(sock);
        bool ok = rr.ok
            && rr.env.body_case() == qtplugin::ipc::Envelope::kHello;
        QString plugin_id;
        if (ok) {
            const qtplugin::ipc::Hello& h = rr.env.hello();
            plugin_id = QString::fromStdString(h.plugin_id());
            ok = (static_cast<int>(h.api_version()) == plugin_ipc::kApiVersion)
              && (static_cast<int>(h.proto_schema_version())
                  == plugin_ipc::kProtoSchemaVersion);
        }
        check(ok, QStringLiteral("Hello handshake"),
              ok ? QStringLiteral("plugin_id=%1").arg(plugin_id) : rr.error);
        mark(ok);
        // 回 HelloAck(关联走 Hello 的 correlation_id)
        qtplugin::ipc::Envelope ack;
        ack.set_correlation_id(rr.env.correlation_id());
        qtplugin::ipc::HelloAck* a = ack.mutable_hello_ack();
        a->set_ok(true);
        a->set_negotiated_schema_version(plugin_ipc::kProtoSchemaVersion);
        send_envelope(sock, ack);
    }

    // 2. ParseRequest × N(只验响应到达 + correlation_id 回填,与旧测试同口径)
    QList<QByteArray> frames;
    if (plugin_name == QStringLiteral("lua_diag")) {
        frames = {
            QByteArray::fromHex("0102"),  // too short
            QByteArray::fromHex("3c000000") + QByteArray(20, '\0'),  // zero TEI
            QByteArray::fromHex("3c000102") + QByteArray(20, '\xAA'),  // OK
        };
    } else {
        frames = {
            QByteArray::fromHex("3c000102") + QByteArray(20, '\xAA'),
            QByteArray::fromHex("3c000201") + QByteArray(30, '\xBB'),
            QByteArray::fromHex("3c000301") + QByteArray(40, '\xCC'),
        };
    }
    quint64 seq = 1;
    for (int i = 0; i < frames.size(); ++i) {
        BplcFrame frame = make_test_frame(frames[i], 1000000 + i * 1000);
        MsduState msdu;
        ParseFilter filter;
        qtplugin::ipc::Envelope env;
        env.set_correlation_id(seq);
        qtplugin::ipc::ParseRequest* req = env.mutable_parse_request();
        *req->mutable_frame() = plugin_proto::to_proto(frame);
        *req->mutable_msdu() = plugin_proto::to_proto(msdu);
        *req->mutable_filter() = plugin_proto::to_proto(filter);
        send_envelope(sock, env);
        RecvResult rr = recv_envelope(sock);
        bool ok = rr.ok
            && rr.env.body_case() == qtplugin::ipc::Envelope::kParseResponse
            && rr.env.correlation_id() == seq;
        QString summary;
        if (ok) summary = QString::fromStdString(
            rr.env.parse_response().result().msdu().summary()).left(60);
        check(ok, QStringLiteral("Parse frame %1").arg(i + 1), summary);
        mark(ok);
        ++seq;
    }

    // 2b. 批解析(仅 js_topo):2 条目,验 item_seq 回填与条目数
    if (plugin_name == QStringLiteral("js_topo")) {
        qtplugin::ipc::Envelope env;
        env.set_correlation_id(seq);
        qtplugin::ipc::ParseBatchRequest* batch = env.mutable_parse_batch_request();
        for (int i = 0; i < 2; ++i) {
            qtplugin::ipc::ParseBatchItem* item = batch->add_items();
            item->set_item_seq(101 + i);
            BplcFrame frame = make_test_frame(frames[i], 2000000 + i * 1000);
            MsduState msdu;
            ParseFilter filter;
            qtplugin::ipc::ParseRequest* req = item->mutable_request();
            *req->mutable_frame() = plugin_proto::to_proto(frame);
            *req->mutable_msdu() = plugin_proto::to_proto(msdu);
            *req->mutable_filter() = plugin_proto::to_proto(filter);
        }
        send_envelope(sock, env);
        RecvResult rr = recv_envelope(sock);
        bool ok = rr.ok
            && rr.env.body_case() == qtplugin::ipc::Envelope::kParseBatchResponse
            && rr.env.correlation_id() == seq;
        if (ok) {
            const qtplugin::ipc::ParseBatchResponse& resp =
                rr.env.parse_batch_response();
            ok = (resp.items_size() == 2)
              && (resp.items(0).item_seq() == 101)
              && (resp.items(1).item_seq() == 102);
        }
        check(ok, QStringLiteral("Parse batch 2 items"),
              ok ? QStringLiteral("item_seq 101/102 echoed") : rr.error);
        mark(ok);
        ++seq;
    }

    // 2c. 心跳 Ping/Pong
    {
        qtplugin::ipc::Envelope env;
        env.set_correlation_id(seq);
        env.set_ping(true);
        send_envelope(sock, env);
        RecvResult rr = recv_envelope(sock);
        bool ok = rr.ok
            && rr.env.body_case() == qtplugin::ipc::Envelope::kPong
            && rr.env.correlation_id() == seq;
        check(ok, QStringLiteral("Ping/Pong"),
              ok ? QStringLiteral("pong corr=%1").arg(seq) : rr.error);
        mark(ok);
        ++seq;
    }

    // 3. 图形测试
    if (test_graphics) {
        // RenderRequest
        {
            qtplugin::ipc::Envelope env;
            env.set_correlation_id(seq);
            qtplugin::ipc::RenderRequest* req = env.mutable_render_request();
            req->set_width(400);
            req->set_height(300);
            send_envelope(sock, env);
            RecvResult rr = recv_envelope(sock);
            bool ok = rr.ok
                && rr.env.body_case() == qtplugin::ipc::Envelope::kRenderResponse
                && rr.env.correlation_id() == seq;
            int img_bytes = 0;
            if (ok) {
                const qtplugin::ipc::RenderResponse& resp =
                    rr.env.render_response();
                QImage img = plugin_proto::image_from_png(
                    QByteArray(resp.png_image().data(),
                               static_cast<int>(resp.png_image().size())));
                ok = resp.ok() && !img.isNull();
                img_bytes = img.width() * img.height();
            }
            check(ok, QStringLiteral("Render 400x300"),
                  ok ? QStringLiteral("%1 px").arg(img_bytes) : rr.error);
            mark(ok);
            ++seq;
        }
        // GraphicsEvent (点击中心)
        {
            GraphicsEvent evt;
            evt.type = GraphicsEventType::MousePress;
            evt.x = 200; evt.y = 150;
            evt.button = 1;
            evt.modifiers = 0;
            evt.delta_y = 0;
            qtplugin::ipc::Envelope env;
            env.set_correlation_id(seq);
            *env.mutable_graphics_event()->mutable_event() =
                plugin_proto::to_proto(evt);
            send_envelope(sock, env);
            RecvResult rr = recv_envelope(sock);
            bool ok = rr.ok
                && rr.env.body_case() == qtplugin::ipc::Envelope::kEventAck
                && rr.env.correlation_id() == seq;
            bool needs_redraw = false;
            if (ok) needs_redraw = rr.env.event_ack().needs_redraw();
            check(ok, QStringLiteral("GraphicsEvent click"),
                  ok ? QStringLiteral("needs_redraw=%1").arg(needs_redraw) : rr.error);
            mark(ok);
            ++seq;
        }
    }

    // 4. Shutdown
    {
        qtplugin::ipc::Envelope env;
        env.set_shutdown(true);
        send_envelope(sock, env);
    }
    sock->disconnectFromServer();
    host.waitForFinished(5000);
    if (host.state() != QProcess::NotRunning) host.kill();

    log() << (all_ok ? "  >> ALL PASS\n" : "  >> SOME FAILED\n");
    return all_ok;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    fprintf(stdout, "DEBUG: app started\n");
    fflush(stdout);
    QCommandLineParser cli;
    cli.addHelpOption();
    QCommandLineOption host_opt(QStringList{QStringLiteral("host")},
        QStringLiteral("path to bplc-plugin-host"), QStringLiteral("bin"));
    QCommandLineOption ex_opt(QStringList{QStringLiteral("examples")},
        QStringLiteral("examples dir"), QStringLiteral("dir"));
    cli.addOption(host_opt);
    cli.addOption(ex_opt);
    cli.process(app);

    const QString host_bin = cli.value(host_opt);
    const QString ex_dir = cli.value(ex_opt);
    if (host_bin.isEmpty() || ex_dir.isEmpty()) {
        QTextStream(stderr) << "usage: plugin_ipc_test --host <bin> --examples <dir>\n";
        return 1;
    }

    log() << "Plugin IPC Integration Test\n";
    log() << "Host: " << host_bin << "\n";
    log() << "Examples: " << ex_dir << "\n";

    bool all_ok = true;
    all_ok &= test_plugin(host_bin, ex_dir + "/js_topo", QStringLiteral("js_topo"), true);
    all_ok &= test_plugin(host_bin, ex_dir + "/lua_diag", QStringLiteral("lua_diag"), false);
    all_ok &= test_plugin(host_bin, ex_dir + "/lua_report", QStringLiteral("lua_report"), false);

    log() << "\n========================================\n";
    log() << QStringLiteral("Total: %1 passed, %2 failed\n").arg(g_pass).arg(g_fail);
    log() << (all_ok ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return all_ok ? 0 : 1;
}
