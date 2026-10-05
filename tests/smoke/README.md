# 框架独立编译冒烟测试

验证 QtPluginFramework 不依赖主程序可独立编译。

```bash
cd tests/smoke
mkdir build && cd build
/path/to/qt/bin/qmake ../framework_test.pro
make -j4
QT_QPA_PLATFORM=offscreen ./framework_test
```

需要 Qt 6（含 Qml 模块）。
