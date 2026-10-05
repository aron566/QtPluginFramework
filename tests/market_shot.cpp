/// @file market_shot.cpp
/// @brief 市场对话框截图探针(临时):打开对话框,等 feed 加载后抓屏
#include <QApplication>
#include <QTimer>

#include "i18n.h"
#include "market_dialog.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("market_shot");
    if (qgetenv("MARKET_EN") == "1") trl::set_enabled(true);
    PluginMarketDialog dlg;
    dlg.show();
    QTimer::singleShot(5000, [&]() {
        dlg.grab().save(QString::fromLocal8Bit(qgetenv("SHOT_OUT")));
        app.quit();
    });
    return app.exec();
}
