# P11 实现记录

状态：P11 代码和专项入口已完成；真实 MCP 宿主双音轨回归通过。固定设备矩阵的长时 CPU A/B 未纳入自动化门禁，仍按宿主受限项记录。

本次实现完成了以下路径收敛：

- 删除固定 250 ms 音轨刷新和永久 500 ms 侧栏维护；音轨/选择变化通过 dirty 位和合并通知触发，必要时使用有界 2 秒恢复窗口。
- `gp_audio` 使用值快照和 generation 比较，缺失音色不再调用 `musician->updateAll()`；对象树读取保留在宿主 Qt 线程。
- selection worker 承接 track runtime 维护、故障隔离、状态保存和采样率重配；VST3 初始化/编辑器生命周期调用按线程合同回到 Qt 线程。
- `hook::snapshot()` 仅读取已发布状态，不再更新音频层或重配输入路由。
- About/侧栏使用共享 pending 调度和有限启动重试，稳定后停止调度。
- `p2-observation.json` 和 `status.json` 使用 latest-slot 后台 writer；正常诊断按状态变化合并写入，详细模式才允许高频采样，退出时先 drain writer 再清理 runtime。
- scanner poll 只在 static future 或 recognition job/queue 活跃时运行，完成后停止 100 ms timer。

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

复核用户截图对应的默认启动路径后确认：旧实现只在 global request 中执行首次 hook `prepare()`，track request 在 hook 尚未安装时直接进入 worker，最终把 `runtime_vst3_selection_prepare_failed` 写入 track sidecar。现已让首次显式 track selection 在 Qt 控制线程完成同样的 hook 安装，并在失败时保留实际原因。

- 默认启动、已安装 Neural DSP Archetype 插件、MCP 选择 Track 2：`installed=true`、`enabled=true`，目标 track `configured=true`、`configured_effects=1`，sidecar 的 Mateus Asato 错误清除。
- 新曲谱双音轨真实 Archetype 处理：`artifacts/mcp-p8-track-0c3318d76a694134a77a5aad2dcca434`。
- 用户曲谱/sidecar 副本复验：`artifacts/mcp-existing-track-dbc482a469274a258b58991f92504bc9`；原始用户数据未写入。
- 同时修复异步 `status.json` writer 覆盖 `plugin_path` 身份字段的问题，避免测试和诊断读取到不属于当前 DLL 的状态。

宿主边界：未声明跨 Guitar Pro 版本的 ABI 兼容；商业插件自身的 editor/线程合同仍由插件实现决定。计划要求的 120 秒×3 固定设备 CPU A/B 需要宿主设备矩阵，不能由夹具结果代替。
