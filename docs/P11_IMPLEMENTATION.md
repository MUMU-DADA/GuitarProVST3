# P11 实现记录

状态：P11 代码和专项入口已完成；真实 MCP 宿主双音轨回归通过。固定设备矩阵的长时 CPU A/B 未纳入自动化门禁，仍按宿主受限项记录。后续真实使用反馈显示，全 catalog 预加载、无曲谱阶段的 global 预加载和定时选择恢复仍需重新收敛，范围转入 [P12 事件驱动切轨与按需运行时计划](P12_EVENT_DRIVEN_LAZY_RUNTIME_PLAN.md)。P11 的专项通过不等同于 P12 目标已完成。

本次实现完成了以下路径收敛：

- 删除固定 250 ms 音轨刷新和永久 500 ms 侧栏维护；音轨/选择变化通过 dirty 位和合并通知触发，必要时使用有界 2 秒恢复窗口。
- `gp_audio` 使用值快照和 generation 比较，缺失音色不再调用 `musician->updateAll()`；对象树读取保留在宿主 Qt 线程。
- selection worker 承接 track runtime 维护、故障隔离、状态保存和采样率重配。勾选启用时，VST3 component/controller 初始化、状态恢复及首次 processing setup 在 worker 执行；factory 对象创建和编辑器挂载保留在 Qt，以兼容 Neural DSP 的 GUI 对象线程归属。勾选响应性证据见 P12 实现记录。
- `hook::snapshot()` 仅读取已发布状态，不再更新音频层或重配输入路由。
- About/侧栏使用共享 pending 调度和有限启动重试，稳定后停止调度。
- `p2-observation.json` 和 `status.json` 使用 latest-slot 后台 writer；正常诊断按状态变化合并写入，详细模式才允许高频采样，退出时先 drain writer 再清理 runtime。
- scanner poll 只在 static future 或 recognition job/queue 活跃时运行，完成后停止 100 ms timer。
- 启动后 selection worker 会按识别成功的 catalog 逐个为 global、每个 track 及显式输入路由准备实例并保持 bypass；显式勾选、停用和切换都复用所属 scope 的实例，不把冷启动时间放在点击路径上。输入路由默认关闭，避免启用 global 时形成输入回音。

验证入口：

```powershell
./native/build.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native/p11-final-build
./native/test/test-p11.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native/p11-final-suite
./native/test/test-p8-runtime.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native/p11-p8-runtime -PluginPath .tools/native/p11-final-build/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/test/test-p8-track-runtime.ps1 -PluginPath .tools/native/p11-final-build/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root .tools/native/p7-gain-test/'P7 Gain Fixture.vst3' -ExpectedBindingSource mcp_context_native_registry -HookMode enabled
```

最终构建 DLL SHA-256：`9CE0F06CFDEC3E4E8F896F08C94AAAB8C3D383E4BC83E821714AF2777C610AD4`。

最近一次证据：`artifacts/p8-runtime-91583ad422634b9694cc157bb730079b`、`artifacts/mcp-p8-track-48b7974f31e34d0cbdde55bf5d51494c` 和 `.tools/native/p11-release-suite-final-host/`。真实流程使用已安装 Guitar Pro 8.1.1.17 与 MCP bridge，没有使用 computer use；宿主安装目录完整性检查通过。

## 2026-09-15 真实 Archetype 音轨复验

复核用户截图对应的默认启动路径后确认：旧实现只在 global request 中执行首次 hook `prepare()`，track request 在 hook 尚未安装时直接进入 worker，最终把 `runtime_vst3_selection_prepare_failed` 写入 track sidecar。首次显式 selection 现在统一排入 selection worker，由 worker 完成 hook 安装和实例准备；Qt 回调只提交请求并立即返回，失败原因仍保留在状态和 sidecar 中。

- 默认启动、已安装 Neural DSP Archetype 插件、MCP 选择 Track 2：`installed=true`、`enabled=true`，目标 track `configured=true`、`configured_effects=1`，sidecar 的 Mateus Asato 错误清除。
- 新曲谱双音轨真实 Archetype 处理：`artifacts/mcp-p8-track-0c3318d76a694134a77a5aad2dcca434`。
- 用户曲谱/sidecar 副本复验：`artifacts/mcp-existing-track-dbc482a469274a258b58991f92504bc9`；原始用户数据未写入。
- 同时修复异步 `status.json` writer 覆盖 `plugin_path` 身份字段的问题，避免测试和诊断读取到不属于当前 DLL 的状态。

## 后台预加载证据

旧预加载实现的用户曲谱/sidecar 副本证据：`artifacts/preload-check-7ea2aaba346d4f309851a78f5e417303`。该轮只覆盖保存为 `desired_enabled` 的条目，不代表完整 catalog 预加载验收。

## 2026-09-15 完整预加载、实例保留和输入回音修复

- 已实现：预加载读取完整 ready catalog，并合并每个 scope 的保存状态；未启用和未写入 sidecar 的插件同样预加载。预加载不受同时启用 8 个效果器的链容量限制；每处理一个插件就让出 worker 给显式选择，单个失败继续处理后续插件。
- 已实现：global、track、显式 input 路由各自持有实例池，双槽只负责当前处理顺序。停用不销毁组件、控制器或模块；切换到其他插件再回来复用原实例和参数。预加载不激活音频，诊断 `instances` 包含全部保留实例及各自的 `active`、`preloaded`、`processed_blocks`。
- 已实现：未设置 `GPVST3_P4_ROUTE` 时输入路由为 `disabled`，勾选 global 不再把输入设备声音送回输出。显式 `input_insert` / `bus_mix` 保留原有路由语义。
- 已验证：`test-p8-runtime.ps1` 使用生产 runtime 和真实 VST3 夹具验证每个 scope 的 9 个插件预加载、失败后继续、预加载期间显式选择、A→B→C→A 多轮启停原实例复用、参数保留、重排后的实际采样结果、采样率重配及状态错误隔离。证据：`artifacts/p8-runtime-21dd2fc0ec63474b92ac5d7560c5f320`。
- 已验证：真实 Guitar Pro + MCP 三插件/双音轨，共 9 个独立实例，6 次切换均保持全部实例 ID；所有 global 启用阶段 `input_route=disabled`、`input_route_enabled=false`、`input_processed_blocks=0`。证据：`artifacts/preload-mcp-5637c18a15f74996a4d59b9f0ea2aafd`。
- 已验证：真实 Archetype Mateus Asato、Archetype Nolly X 在 global 和两音轨预加载共 6 个独立实例，global/track 分别 A→B→A 后 ID 不变，激活插件实际处理块增加，输入监听保持关闭。证据：`artifacts/preload-mcp-09ab93e4dc704bc2b1d30dd1c432a9b8`。商业插件批量初始化可能超过 30 秒，宿主测试为此使用 120 秒上限。
- 已验证：生产 DLL 构建、PowerShell 语法、`git diff --check`、P11 调度/UI/activation 和 P4 router 专项；宿主测试使用独立数据目录，安装目录完整性检查通过。
- 宿主受限：旧 `test-p8-order.ps1` 新建曲谱流程在本次及修改前 DLL 都出现没有新的处理回调/读取旧块结果，未计为通过；本次顺序正确性由生产 runtime 的确定性采样回归验证。设备听感和跨设备矩阵仍未验收。

宿主边界：未声明跨 Guitar Pro 版本的 ABI 兼容；商业插件自身的 editor/线程合同仍由插件实现决定。计划要求的 120 秒×3 固定设备 CPU A/B 需要宿主设备矩阵，不能由夹具结果代替。
