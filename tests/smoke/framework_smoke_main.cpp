// 框架独立编译冒烟测试:实例化核心类,验证头文件自包含
#include "local_plugin_engine.h"
#include "plugin_manager.h"
#include "plugin_market.h"
#include "iplugin.h"
#include "bplcframe.h"
#include "iprotocolparser.h"
#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    LocalPluginEngine engine;
    engine.set_ui_dark(false);
    PluginManager& mgr = PluginManager::instance();
(void)&mgr;
    BplcFrame f{};
    (void)f;
    return 0;
}
