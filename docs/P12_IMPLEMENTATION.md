# P12 实现记录

状态：代码和专项验收完成；真实 ASIO/WASAPI 听感、其他 Guitar Pro 版本及长时设备矩阵仍属于宿主受限项（2026-09-16）。

本阶段把曲谱生命周期、选择上下文和音频图维护拆开，默认路径不再在主界面或 catalog 扫描完成时创建 VST3 实例。

## 已实现

- `gp_audio` 增加独立的 `selection_generation`、`binding_generation`、选择发布延迟和合并刷新计数。拓扑比较忽略 `activeDocument/selectedTrack`，纯切轨不会重新发布音频 dispatch。
- Qt observer 将 document view、cursor/selection 相关动态属性和文档重建事件转成可合并的 selection/topology dirty 事件。桥接暂不可用时保留 dirty 状态并重试，不清除待处理事件。
- fallback timer 改为文档/重建后的有界恢复窗口，最多 8 次、每次 250 ms；无曲谱或窗口耗尽后自动停止，不再使用进程级静态计数。
- 启动不再调用 `disableAllEffectsAtStartup()` 覆盖 sidecar。用户配置的 `enabled/order/state` 保持在 sidecar，运行时仍以 bypass/active 状态独立启动。
- `preloadSavedSelections()` 只在存在 active document 且 scope 中明确 `enabled=true` 时排队；catalog 中未配置或已停用的模块保持 metadata-only。没有打开曲谱时清空旧 preload 请求，不创建 processor。
- 识别结果使用 latest-slot `CatalogCacheWriter` 在后台写入，Qt poll 只交付内存快照；超时/成功结果不会阻塞宿主事件循环。
- 旧 `desired_enabled` 运行时标记通过 `migrateDesiredEnabledIntent()` 一次性转换为用户的 `enabled/order/state` 意图，启动阶段不再反复覆盖 sidecar。
- TrackRuntime 增加原子 `bypassRequested` 和固定 scope key hash。停用请求不等待 selection mutex，下一次 audio callback 直接旁路；插件准备完成后再清除 bypass。
- 每个 scope 的 `EffectPool` 设置 16 个 warm-cache 实例上限，只淘汰未被 slot 引用的 dormant preload，并通过 `warm_cache_limit/warm_cache_evictions` 暴露结果。
- 增加按 `selection_generation` 和当前 `trackKey` 双重校验的 `requestTrackVst3SelectionAtGeneration()`，过期请求返回 `stale_selection_generation`，不会写入另一条音轨。
- 无曲谱时不创建 track/global P7 panel；曲谱上下文确认后再挂载 native section。

## 验证证据

已运行：

```powershell
./native/build.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native/p12-release-final
./native/test/build-p7-gain-fixture.ps1 -OutputRoot .tools/native/p12-gain-fixture
./native/test/test-p12-lazy-startup.ps1 -PluginPath .tools/native/p12-release-final/plugins/imageformats/guitarpro_vst3_autoload.dll -OutputRoot .tools/native/p12-lazy-startup-final2
./native/test/test-p8-track-runtime.ps1 -PluginPath .tools/native/p12-release-final/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root '.tools/native/p12-gain-fixture/P7 Gain Fixture.vst3' -ExpectedBindingSource mcp_context_native_registry -HookMode enabled -CheckP12
./native/test/test-p8-track-runtime.ps1 -PluginPath .tools/native/p12-release-final/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root '.tools/native/p12-gain-fixture/P7 Gain Fixture.vst3' -ExpectedBindingSource mcp_context_native_registry -HookMode enabled -CheckLifecycle
./native/test/test-p11.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native/p12-p11-final
git diff --check
```

P8 runtime 夹具已通过以下 P12 回归：只预加载已启用 scope、stale generation 拒绝、停用旁路、异步启用、状态保留、采样率重配、失败隔离、编辑器生命周期和输入路由默认关闭。P12 集成套件已通过：`.tools/native/p12-release-suite5`。其中包含无曲谱启动、P11 调度/UI/激活、A→B→A 选择代际、双音轨实例与状态、复制/交换/撤销/保存重开/进程重启和最终宿主完整性检查。识别缓存 writer 和超时队列分别由 `p12-recognition2`、`p12-recognition-timeout5` 验证，静态 catalog 由 `p12-p7-catalog` 验证。

## 宿主边界

当前已在匹配的 Guitar Pro 8.1.1.17 + MCP 会话中观察到 `selection_hook_installed=true`、`selection_hook_gate_passed=true` 和 `selection_event_source=native_cursor_hook`；未通过 host hash/prologue gate 时保持安全旁路。其他 Guitar Pro 版本、真实 ASIO/WASAPI 听感、第三方进程级崩溃隔离和长时 CPU A/B 不在本阶段自动门禁内。
