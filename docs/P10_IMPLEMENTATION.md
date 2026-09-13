# P10 实现记录

状态：已实施；真实 Guitar Pro UI 回归通过（目标版本 `v0.9.8`，2026-09-13）。
关联计划：[P10 editor/音频生效优化计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)

## 修复内容

### 原生 editor

- `RuntimeEffect` 的 component、factory/controller 创建和初始化通过 `invokeOnQtThreadBlocking` 在 Guitar Pro Qt 主线程完成，保留第三方插件建立 Qt 对象所需的线程归属。
- editor 使用独立持久线程执行 `createView`、`isPlatformTypeSupported`、`setFrame`、content scale、`getSize` 和 `attached`。Qt 主线程继续处理事件，因此 Mateus 的 `attached()` 不再把宿主卡死。
- editor host 保持 `WA_NativeWindow`/稳定 HWND；`RuntimePlugFrame::resizeView` 将宿主窗口尺寸更新转发回 Qt 线程，再调用 `IPlugView::onSize`。
- 关闭时先通知 editor 线程执行 `removed`/`setFrame(nullptr)`，Qt 线程用嵌套事件循环等待线程结束，避免第三方 view 在销毁时回调 Qt 造成死锁。关闭 editor 只移除 view，不停用音频实例。

### 音频和诊断

- 保留 P10 已有的 selection generation、准备/提交时间戳、首个处理 callback 和 bypass 观测字段。
- `editor_stage` 和 `editor_result_code` 覆盖 `create_view`、`platform_check`、`set_frame`、`get_size`、`attached`、`visible`、`removed`、`failed` 等阶段；失败仍保持中性 UI 文案。

## 真实 MCP 验证

验证使用已安装的 MCP bridge 驱动 Guitar Pro 8.1.1.17，未使用 computer use。命令入口为：

```powershell
.\native\test\test-p7-mcp.ps1 -EditorOnly -HookMode default `
  -PluginPath .tools/native/p10-hybrid-editor-build/plugins/imageformats/guitarpro_vst3_autoload.dll `
  -McpRoot C:\Users\mumu\source\GuitarProMCP `
  -Vst3Root 'Neural DSP/Archetype Mateus Asato.vst3;Gateway.vst3'
```

通过证据目录：`artifacts/mcp-p7-c759c17ee0b44595a298f47c5314752e`。

- 窗口标题为 `Archetype Mateus Asato · VST3`，尺寸 `1510x1153`，`non_modal=true`，`editor_stage=visible`，`editor_error=""`。
- 重复触发复用同一 `window_id`，窗口仍可见；关闭后 `editor_stage=removed`，Guitar Pro 保持响应。
- [editor-native.png](../artifacts/mcp-p7-c759c17ee0b44595a298f47c5314752e/editor-native.png) 是按 Guitar Pro PID 取得的 native `PrintWindow` capture，内容可见，不是白屏。
- [editor-trace.log](../artifacts/mcp-p7-c759c17ee0b44595a298f47c5314752e/data/editor-trace.log) 记录了 `create_view → set_frame → get_size → attached → visible → removed` 的前后顺序；`attached` 发生在线程 `35996`，Qt 主线程线程号为 `33528`。
- 关闭后音频观测仍有 `101` 个 global process blocks、`219` 个 input blocks、首个处理 callback 为 `1`、`chain_sequence_gaps=0`。

## 夹具验证

已通过：

```powershell
.\native\test\test-p10-activation.ps1 -OutputRoot .tools/native/p10-activation-final
.\native\test\test-p8-runtime.ps1 -QtDir C:\Users\mumu\source\GuitarProMCP\.tools\qt\5.15.2\msvc2019_64 `
  -OutputRoot .tools/native/p10-runtime-final2 `
  -PluginPath .tools/native/p10-hybrid-editor-build/plugins/imageformats/guitarpro_vst3_autoload.dll
```

`test-p10-editor.ps1` 和 `test-p10.ps1` 已改为默认使用 Mateus + Gateway，并在 MCP 流程中明确执行 editor-only 的打开、重开和关闭断言。

## 未覆盖项

真实 ASIO/WASAPI 设备矩阵、不同 Guitar Pro 版本、真实扬声器听感阈值，以及没有合法 HWND editor 的第三方插件仍属于宿主或插件限制。某些插件要求 `createView` 也必须在其 Qt 线程执行；这类插件会保留诊断阶段并失败回退，不能由宿主伪造 GUI 成功。
