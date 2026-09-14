# P11 实现记录

状态：代码实现完成；静态、维护、UI、激活和电平检测专项通过。真实宿主的声音验收现在要求冷启动完成后的新鲜 peak/RMS/AC RMS 电平窗口；未通过该窗口的宿主回归不得标记为声音能力完成。

## 已实现

- 删除固定 250 ms 音轨刷新、永久 500 ms 侧栏维护和 autoload 中重复的 scanner timer。
- `gp_audio` 使用 dirty 位、双缓冲值快照和选择上下文增量更新；对象树/宿主模型读取只在 Qt 线程执行，缺失音色不再调用 `musician->updateAll()`。
- `refreshTrackContext()` 只收集并提交不可变 binding 值；selection worker 承接 track runtime 的准备、故障隔离、状态保存、退役和采样率重配，完成后通过合并通知刷新 UI。
- `hook::snapshot()` 只读取已发布状态，不再更新音频层或重配输入路由；`g_bindingSource` 和 dispatch 读取使用原子/reader generation 保护。
- 曲谱/RSE 重建替换原生 `Track` 对象时，音轨 native id 变化会按当前文档和音轨序号回退到已有持久化 key；同一轮已知音轨 key 先保留，避免新插入音轨抢占旧链。
- About/侧栏挂载使用共享 pending 调度、QPointer/父子关系复用和有限退避；稳定窗口停止维护，隐藏面板不执行周期扫描。
- `p2-observation.json` 使用 normal/detailed 两种模式、generation 和有界 latest-slot writer；normal 模式状态变化按秒合并，纯计数遥测最多每 5 秒提交一次，Qt 线程不执行 QSaveFile。
- `status.json` 的 scanner poll 结果通过单槽后台 writer 提交；退出时 writer join，避免旧快照覆盖新 generation。
- `audio-level-check.ps1` 只接受冷启动完成后、module/class/instance/generation/request 一致的连续电平样本；4 秒窗口要求 VST3 输入与输出均有有效 AC 电平和至少 3 dB 的分位变化，同时检查最终设备缓冲。处理块计数和 hash 写回只作为诊断字段。
- scanner 仅在 static future/recognition job 活跃时启动 100 ms poll；音轨 fallback 只在启动或重建 settling window 内检查，稳定后停止。

## 验证证据

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 生产 DLL | 通过 | `.tools/native/p11-build-release/plugins/imageformats/guitarpro_vst3_autoload.dll` |
| P11 maintenance/UI/activation 套件 | 通过 | `.tools/native/p11-suite-release-final2/` |
| P8 runtime、selection、editor fixture | 通过 | `artifacts/p8-runtime-caed9fb1a64a4dbebd7a1cb21bfd4d32` |
| 真实 Guitar Pro 双音轨 MCP 回归 | 通过 | `artifacts/mcp-p8-track-b25b753d08fe4add8fd166350559f459` |
| P11 套件含真实宿主 | 通过 | `.tools/native/p11-suite-release-final2/verification.json` |

真实回归使用已安装 Guitar Pro 8.1.1.17、MCP bridge、`ParametricOD.vst3;Gateway.vst3`，验证两条独立 track runtime 均完成处理和 writeback，并覆盖 track/global UI scope 切换。测试没有使用 computer use。

上述旧回归证据只证明当时的处理/writeback 路径；从电平门槛启用起，必须重新运行带 `GPVST3_DIAGNOSTIC_MODE=detailed` 的 `test-p8-track-runtime.ps1`/`test-p11.ps1 -RunHost`。准备超时和电平采样不足会分别报告，不能用旧 evidence 或 `process_blocks` 替代。

最终发布 DLL SHA-256：`804F9E8C7867564DE39B1E60C49609249B9FB764E793BD6AE8A8B7A1C17A15DF`。

## 宿主边界和未完成采样

- 真实宿主的 MCP/Qt 生命周期事件覆盖已通过本机双音轨流程；未声明跨 Guitar Pro 版本的 ABI 兼容。
- 音轨对象可能在宿主异步重建期间暂时只有 context 没有 EffectsChain，代码使用最多 5 次、2 秒间隔的 recovery；稳定后的初始 settling window 结束即停止，超过窗口会停止并保持旁路。
- 计划要求的固定 A/B 场景主线程 CPU、P95 回调和 120 秒重复采样尚未在本机自动化采集；这些数值不能由 fixture PASS 推导。`status.json`、`p2-observation.json` 已保留 generation、计数和时间字段供后续采样。
- 第三方 VST3 自身初始化/editor 的耗时和 Guitar Pro 私有模型阻塞仍属于宿主/插件边界，P10 fixture 已验证本地 RuntimeEffect 生命周期。
- 本轮单独重跑 `test-p10-editor.ps1` 时，MCP `gp_objects` 在 editor 触发后的 15 秒 HTTP 超时；该失败证据保留在最新 `artifacts/mcp-p7-*`，不把它误报为 P11 通过项。P11 真实双音轨流程和本地 editor fixture 均已通过。

## 运行入口

```powershell
./native/build.ps1 -OutputRoot .tools/native/p11-build
./native/test/test-p11.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64
./native/test/test-p11.ps1 -RunHost -PluginPath .tools/native/p11-build/plugins/imageformats/guitarpro_vst3_autoload.dll
```

## 2026-09-14 回归修正

P11 的第一次实现把 `refreshTrackContextImpl()` 的链维护移到了 selection worker，
但同时把 `AudioLayer`/`AudioCore` 私有 getter 和首次 hook 安装也留在了 worker。
这些调用要求 Guitar Pro 的控制线程或音频线程，导致宿主 RSE 更新等待、MCP/UI 请求超时，
并使曲谱播放中的 VST3 链停在 `preparing` 或旁路状态。

本次修正将采样率和音频层状态改为控制线程发布的原子快照；首次显式选择在 Qt 控制线程完成
hook 安装，worker 只负责 VST3 实例准备和链切换。上下文维护不再覆盖终态选择状态，
成功选择也不再重建整个 Qt `QListWidget`，避免交互时销毁刚观察到的控件；高频 `LayoutRequest`
事件不再触发全量收集，文档视图的 `Show/Hide` 只更新选中上下文；稳定会话的兜底扫描仅在有限
settling window 内运行，并在编辑器打开时暂停。

此前修正的验证证据（处理/writeback 专项，不代替当前电平验收）：

- `.tools/native/p11-final-fix5/plugins/imageformats/guitarpro_vst3_autoload.dll` 编译通过。
- `native/test/test-p11.ps1` 的 static、maintenance、UI、activation 套件通过。
- `native/test/test-p8-runtime.ps1` 通过，并新增断言 selection worker 不调用线程亲和的宿主音频 getter。
- 真实 Guitar Pro 默认启动、首次勾选后的曲谱播放通过：`installed=true`、`selection_status=applied`、
  `chain_active_slot=0`、`global_chain_process_blocks=44`、`runtime_process_observed=true`、
  `runtime_buffer_write_observed=true`，证据目录为 `.tools/native/default-playback-check-final5`。
- 真实双音轨回归通过，证据目录为 `artifacts/mcp-p8-track-39a19064b65f4049860c59892cfa103f`。

完整 P7 脚本在 ParametricOD 原生 editor 后仍可能受到第三方 editor/宿主自身的 MCP 超时影响；
该路径不作为本次用户已确认的曲谱播放验收依据。

发布包使用 `native/package.ps1`，包内包含本记录和 P11 计划；构建产物、MCP session、宿主配置和 `artifacts/` 均不提交。

## 2026-09-14 音轨输入/输出电平验收

用户确认故障范围是“曲谱播放 → 音轨 VST3”，涉及该路径上的所有插件。全局曲谱链或设备输出有电平，不作为音轨通过的证据。

- 已验证：新 DLL 构建、原生 adapter 输入/输出电平测量、RuntimeEffect 先有声后全零更新，以及 21 种无效电平证据的拒绝逻辑；冷启动等待与实际采样窗口分别计时。
- 未通过：`test-p8-track-runtime.ps1` 使用真实 Guitar Pro 和 `ParametricOD.vst3;Gateway.vst3`，等待就绪后开始 4 秒窗口。两条音轨各只取得 2 个新鲜输入/输出电平样本，之后停止更新；同一窗口设备端取得 16 个样本。检测明确返回失败，没有沿用历史电平或累计处理次数。
- 最新失败证据：`artifacts/mcp-p8-track-aea8fed386e745b19e0c1710c87ab407/audio-levels.json`；原生 runtime 夹具证据：`artifacts/p8-runtime-cc89e424ae664373a4d1620d7d52b943`；电平门槛/冷启动反例：`.tools/native/audio-level-gate/`。

本轮修改检测与诊断，没有修复音轨输入流中断。采样中断的原因仍需结合当前音轨绑定与宿主回调继续定位；不能把本轮结果表述为音轨声音能力已完成。
