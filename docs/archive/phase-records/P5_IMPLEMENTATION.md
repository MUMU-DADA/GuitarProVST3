# P5 实现记录：Qt 界面和 sidecar 状态

P5 已完成最小 Qt 效果器链编辑器和 sidecar JSON 状态保存。插件通过现有 Qt 自动加载入口启动面板；面板在确认存在 `gp::gui::MainWindow` 时挂载到右侧 dock，并在 `soundsContainer` 中增加“VST3 效果器链”入口。未找到稳定的 GP 私有控件插入 ABI 时，仍保留独立 Qt 面板，不修改 GP 私有对象布局。

## 已实现

- `native/modules/qt_ui.cpp/.h`
  - 链项列表、添加/删除、上移/下移、搜索、旁路、重新选择插件和参数表编辑。
  - 记录曲谱标识、`track`、`bus`，并保存 `plugin_path`、`class_uid`、`parameters`、`state_chunk` 和 `bypass`。
  - 启动读取 sidecar；插件路径不存在时自动显示旁路并保留重新选择入口。
  - 通过现有 Qt 主线程创建窗口；没有在实时回调中创建 QWidget 或访问磁盘。
- `native/modules/state_manager.cpp/.h`
  - `effect-chain.json` 使用 `QSaveFile` 原子写入。
  - 校验 `schema=1` 和 `effects` 数组；损坏或不支持的 sidecar 回退为空链并返回错误文本。
  - 默认目录仍为 `%LOCALAPPDATA%/GuitarProVST3`，测试可用 `GPVST3_DATA_DIR` 隔离。
- `native/build.ps1`
  - 将 `qt_ui.cpp` 纳入构建并链接 `Qt5Widgets.lib`。
- `native/test/test-p5-state.ps1`、`native/test/test-p5-ui.ps1`
  - 覆盖 sidecar 字段往返、损坏 JSON 回退、面板创建、缺失插件行和核心控件。

## 验证

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.2/msvc2019_64
./native/test/test-p5-state.ps1 -QtDir C:/path/to/Qt/5.15.2/msvc2019_64
./native/test/test-p5-ui.ps1 -QtDir C:/path/to/Qt/5.15.2/msvc2019_64
./native/test/test-p0.ps1 -PluginPath .tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll
```

本次已通过 MSVC x64 构建、sidecar 隔离夹具、Qt offscreen 面板夹具和真实 Guitar Pro P0 启动/卸载回归。真实 GP 中的 VST3 编辑器窗口仍由第三方 VST3/宿主接口负责，当前面板不伪造编辑器；GP 私有控件布局升级后的 dock 位置和音源区入口需要重新观察。

## 边界

- sidecar 的 `score_id` 默认从 `GPVST3_SCORE_PATH` 或用户面板字段取得；GP 未公开稳定的当前文档通知 ABI，因此不会猜测当前曲谱并覆盖错误文件。
- 当前 sidecar 配置与 P3 已验证运行时链分离；旁路状态先持久化到 sidecar，多实例链的运行时原子重建留在后续版本。
- 参数表保存任意 `ParamID` 字符串和值，实际第三方插件参数控制器和编辑器窗口仍由插件自身提供；未通过私有 ABI 宣称已完成 GP 原生编辑器嵌入。
