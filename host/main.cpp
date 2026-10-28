/// @file main.cpp
/// @brief bplc-plugin-host 入口:用法 bplc-plugin-host --socket <name> --plugin <dir>
/// @note 必须用 QGuiApplication:host 渲染图形(QImage/QPainter/QPixmap/
 ///      QFontDatabase),QCoreApplication 下渲染时 SIGABRT(实测)。
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QTextStream>

#include <vector>

#include "plugin_host.h"

int main(int argc, char* argv[]) {
    // QGuiApplication 会在构造时吃掉 --plugin <值>(当作平台插件名,报
    // "No such plugin for spec");对外接口仍接受 --plugin,这里在构造
    // app 前把它改写为 --plugin-dir,解析器只认 --plugin-dir。
    std::vector<QByteArray> argv_buf;
    std::vector<char*> argv_new;
    for (int i = 0; i < argc; ++i) {
        QByteArray a(argv[i]);
        if (a == "--plugin") a = "--plugin-dir";
        argv_buf.push_back(a);
    }
    for (auto& a : argv_buf) argv_new.push_back(a.data());
    argv_new.push_back(nullptr);
    int argc_new = argc;
    QGuiApplication app(argc_new, argv_new.data());

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("BPLC plugin host process"));
    cli.addHelpOption();
    QCommandLineOption socket_opt(QStringList{QStringLiteral("socket")},
        QStringLiteral("local socket name"), QStringLiteral("name"));
    QCommandLineOption plugin_opt(QStringList{QStringLiteral("plugin-dir")},
        QStringLiteral("plugin directory"), QStringLiteral("dir"));
    cli.addOption(socket_opt);
    cli.addOption(plugin_opt);
    cli.process(app);

    const QString socket_name = cli.value(socket_opt);
    const QString plugin_dir  = cli.value(plugin_opt);
    if (socket_name.isEmpty() || plugin_dir.isEmpty()) {
        QTextStream(stderr) << "usage: bplc-plugin-host --socket <name> --plugin <dir>\n";
        return 1;
    }

    PluginHost host(socket_name, plugin_dir);
    QString err;
    if (!host.start(&err)) {
        QTextStream(stderr) << "plugin-host start failed: " << err << "\n";
        return 2;
    }
    return host.exec();
}
