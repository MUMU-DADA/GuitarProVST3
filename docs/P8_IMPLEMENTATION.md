# P8 实现记录

P8 当前仍不能整体标记为完成（2026-09-11）。P8.7、P8.9、P8.10 和 P8.11 已实现并通过专项夹具及真实 Guitar Pro MCP 对象树验证；P8.8 的真实音轨运行时链仍受宿主私有 ABI 限制，继续安全旁路。补齐任务和验收边界见 [P8 计划](P8_TRACK_GLOBAL_VST3_PLAN.md) 的“P8 补齐计划：六项回归问题（P8.7–P8.12）”。

## 已实现

- `vst3_catalog` 在静态扫描完成后建立单 bundle 识别队列，同一时间只运行一个 factory 任务。队列可被 `shutdownScan()` 排空；静态扫描的 `modules_loaded`/`instances_created` 计数与主动识别计数分开。缓存记录新增 `recognition_status`、`recognition_source`、`recognition_attempts`、`recognition_error`、`recognition_retry_after`、`recognition_deadline_at`、`recognition_ignored_reason`、`recognition_scanner_version`。
- UI 勾选待识别候选只显示“后台识别中”，不调用同步识别函数；识别完成后 `pollVst3()` 合并目录并刷新面板。入口按钮显示“后台识别中…”。
- `state_manager` 读写 schema 2：`global.effects` 保存 Master 链，`scores.<score>.tracks.<track>.effects` 保存音轨链。schema 1 的顶层 `effects` 迁移到 global 并保留旧视图；每个 effect 保留 opaque state、enabled/bypass 和 order。
- P7 面板保留原入口 `gpvst3P7Panel`，新增 `当前音轨`/`全局 Master` tabs，稳定列表 objectName 为 `gpvst3TrackChainList`、`gpvst3GlobalChainList`、`gpvst3AvailableList`。启用项按保存顺序排在可用项前，列表启用内部拖动并立即保存 order。
- `gp_hook` 新增 `setGlobalVst3Selection`、`setTrackVst3Selection`、`captureGlobalVst3States`、`captureTrackVst3States`。global 继续使用已验证的 Master 后处理双槽链；`processDSP` 记录 self/buffer/线程观测，并额外记录锁定宿主上 `EffectsChain::index()` 的只读诊断值。该值是效果链实例索引，不等同于音轨 ID；未取得稳定 track context 时设置 `track_scope_unresolved=true`，不会把音轨效果广播到其他音轨。

## 验证

```powershell
./native/test/test-p8-state.ps1
./native/test/test-p8-recognition.ps1
./native/test/test-p8-ui.ps1
./native/test/test-p7-catalog.ps1
./native/test/test-p7-ui.ps1
./native/build.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native/p8-build
git diff --check
```

上述夹具分别验证 schema 1→2 迁移及 opaque state、单 worker 后台识别和缓存证据、10 秒 watchdog、双 scope 独立状态/启用前置/顺序保存、窄宽度行布局；P7 静态目录与 UI 回归继续通过。完整原生构建已生成 DLL。

真实宿主 MCP 回归使用 Guitar Pro 8.1.1.17（隔离运行目录 `.tools/native/p8-host-live7`）：双音轨播放观察到 `effects_chain_processDSP.call_count=138`、`effects_chain_context_count=2`、`effects_chain_index_observed=true`，但 `track_context_key` 为空且 `track_context_stable=false`，因此没有发布音轨 VST3 runtime chain。对象树同时确认 `gpvst3TrackVst3Section` 位于 `soundRack`（y=225、h=81）之后（y=312），`gpvst3GlobalVst3Section` 位于 `soundMastering`（y=312、h=76）之后（y=394），两个 `QFrame` 分界线可读；`gpvst3SoundEffectChainButton` 仍由 `soundsContainer` 持有。该目录和 JSON 仅为本地证据，不纳入提交。

## 宿主受限项

当前锁定的 Guitar Pro 8.1.1.17 私有 `EffectsChain::processDSP` ABI 可观测到 `self`、`IAudioBuffer`、调用时序以及 `EffectsChain::index()`；真实双音轨播放出现多个链实例，但该 index 只是链实例索引，`self -> GP track ID` 生命周期映射仍未确认。因此 track chain 仅保存 desired state 并旁路，不能声称已完成真实多音轨声学处理。主动识别超时没有第三方安全取消 ABI，超时后 worker 会 detached 并丢弃结果，以保证 GP/UI 不被第三方 DLL 卡住。global 与 P4 capture 的最终相对顺序保持既有验证结果；capture 纳入 global bus、真实扬声器听感、设备差异和第三方崩溃恢复仍需宿主专项验证。

## P8 未完成项（当前回归结论）

- 音轨链仍因缺少稳定 `self -> track_key` 映射而旁路；schema 2 中存在的音轨 desired state 不代表音频已处理。
- global 与 P4 capture 的最终相对顺序、真实扬声器听感、设备差异和第三方崩溃恢复仍需宿主专项验证。

P8 在音轨运行时映射通过真实宿主验收前继续保持“未完成”；已通过的 P8.7、P8.9–P8.11 不改变这一整体状态。
