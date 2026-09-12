# P10 实现记录

状态：已实施并通过夹具、Qt 原生窗口和 MCP 宿主回归（2026-09-13）
关联计划：[P10 editor/音频生效优化计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)
基线：`v0.9.6` / Guitar Pro 8.1.1.17 / Windows x64。

## 交付内容

| 范围 | 状态 | 实现与证据 |
| --- | --- | --- |
| P10-0 时间线诊断 | 已实现、已验证 | `selection_request_id`、queued/worker/prepared/committed 时间戳、`audio_generation`、首个处理块序列和 callback 证据写入 `gp_hook` 状态。过期 global/track 请求在提交前丢弃。 |
| P10-1 原生 editor | 已实现、已验证 | `RuntimeEffect::openEditor` 固定 `createView → platform → setFrame → scale/size → attached → onSize` 顺序；记录 `editor_stage`、原始 `editor_result_code`、identity 和失败原因。Qt 侧使用稳定 `WA_NativeWindow` 子 HWND，关闭 editor 只移除 view。 |
| P10-2 首次启停生效 | 已实现、已验证 | 空选择立即设置 global/input/track bypass；输入和 playback 双槽保留 warm slot，匹配 module/class/state/rate 时复用实例；`Chain` 记录 activation 到首个成功处理块的时间和序列。 |
| P10-3 UI 状态 | 已实现、已验证 | 复选框立即显示请求中或已旁路；worker 完成后同步“已生效/失败已恢复”。editor 等待期间只保留最后一个请求，完成通知由维护 tick 触发一次重试，不再使用固定 50 ms 轮询。 |
| P10-4 回归与发布 | 已实现、已验证 | `test-p10-activation.ps1` 验证立即旁路、warm/cold 首块和连续切换；`test-p10-editor.ps1` 串联生产 RuntimeEffect、Qt/HWND 夹具及 MCP 宿主 editor/关闭/重开流程。 |

## 诊断字段

`gp_hook.selection_*` 描述一次选择请求从排队到提交的阶段；`selection_status` 为 `idle`、`queued`、`preparing`、`applied` 或 `failed`。`audio_generation` 在 global/input 或 track 提交后递增。`chain_*first_processed*` 与 `input_*first_processed*` 只在新 slot 首次成功写回时记录，宿主没有 callback 时保持 0。

`editor_stage` 取 `requested`、`busy_wait`、`controller_missing`、`create_view`、`platform_check`、`set_frame`、`get_size`、`attached`、`visible`、`focus`、`removed` 或 `failed`；`editor_result_code` 保留 VST3 `tresult`。详细 identity 和错误只进入结构化状态，不进入产品中性文案。

## 验证记录

已执行并通过：

```powershell
.\native\test\test-p10-activation.ps1 -OutputRoot .tools/native/p10-activation-check2
.\native\test\test-p8-runtime.ps1 -QtDir C:\Users\mumu\source\GuitarProMCP\.tools\qt\5.15.2\msvc2019_64 -OutputRoot .tools/native/p10-runtime-check -PluginPath .tools/native/p10-build/plugins/imageformats/guitarpro_vst3_autoload.dll
.\native\test\test-p10-editor.ps1 -QtDir C:\Users\mumu\source\GuitarProMCP\.tools\qt\5.15.2\msvc2019_64 -OutputRoot .tools/native/p10-editor-check -PluginPath .tools/native/p10-build/plugins/imageformats/guitarpro_vst3_autoload.dll -McpRoot C:\Users\mumu\source\GuitarProMCP -Vst3Root 'ParametricOD.vst3;Gateway.vst3'
```

最终 MCP 证据目录为 `artifacts/mcp-p7-9ba57930ba784b16b539ab1a9b92f27a`：原生 editor 窗口可见、非 modal、可关闭和重开；`editor_stage=visible`；停用后 `total_bypass=true`；`chain_sequence_gaps=0`、首个处理块 callback 为 1，选择提交时间线均已写入结构化观测。生产 RuntimeEffect 夹具证据为 `artifacts/p8-runtime-094910ae82aa4143a6e145417f2d228d`，完整套件目录为 `.tools/native/release-p10-suite`。

未覆盖项仍按宿主边界记录：真实 ASIO/WASAPI 设备矩阵、真实扬声器听感阈值、没有合法 editor 的第三方插件，以及第三方插件进程级崩溃隔离。此类插件现在会返回稳定的 `editor_stage`/`editor_result_code` 并保持旁路，不伪造 GUI 成功。
