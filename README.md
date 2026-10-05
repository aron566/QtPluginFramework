# QtPluginFramework

通用的 Qt 插件框架（从 HPLC_Wireshark 剥离）。

## 组成

- `plugin_api/` — 插件接口定义（IPlugin、IProtocolParserPlugin、IGraphicsPlugin、manifest）
- `host/` — 插件宿主（JS/Lua/native 三种运行时后端）
- `manager/` — 插件管理器（加载/卸载/代理）
- `market/` — 插件市场（feed/下载/校验/安装/更新）
- `ipc/` — 进程间通讯序列化
- `app_integration/` — 主程序集成（LocalPluginEngine、PluginWorker、面板）
- `include/common/` — 共享数据模型（BplcFrame、PhysicalMeta）与国际化
- `include/protocol/` — 协议解析接口（IProtocolParser、ParseResult）
- `examples/` — 示例插件

## 依赖

- Qt 6 (Core, Network, Widgets, Qml for JS backend)
- Lua 5.4（`3rdparty/`，可选）
- miniz（`3rdparty/`，市场解包用）

## 接入

主程序 `.pro` 中：

```qmake
PLUGIN_FW = $$PWD/3rdparty/QtPluginFramework  # 或 submodule 路径
include($$PLUGIN_FW/plugin.pri)
INCLUDEPATH += $$PLUGIN_FW/include/common $$PLUGIN_FW/include/protocol
```

## 许可证

与主项目一致。
