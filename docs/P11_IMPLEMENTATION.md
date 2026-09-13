# P11 实现记录

状态：代码实现完成，专项和真实 MCP 宿主回归通过；主线程 CPU 长时 A/B 门槛仍需在固定设备矩阵上采样（2026-09-13）。

## 已实现

- 删除固定 250 ms 音轨刷新、永久 500 ms 侧栏维护和 autoload 中重复的 scanner timer。
- `gp_audio` 使用 dirty 位、双缓冲值快照和选择上下文增量更新；对象树/宿主模型读取只在 Qt 线程执行，缺失音色不再调用 `musician->updateAll()`。
- `refreshTrackContext()` 只收集并提交不可变 binding 值；selection worker 承接 track runtime 的准备、故障隔离、状态保存、退役和采样率重配，完成后通过合并通知刷新 UI。
- `hook::snapshot()` 只读取已发布状态，不再更新音频层或重配输入路由；`g_bindingSource` 和 dispatch 读取使用原子/reader generation 保护。
- About/侧栏挂载使用共享 pending 调度、QPointer/父子关系复用和有限退避；稳定窗口停止维护，隐藏面板不执行周期扫描。
- `p2-observation.json` 使用 normal/detailed 两种模式、generation 和有界 latest-slot writer；normal 模式状态变化按秒合并，纯计数遥测最多每 5 秒提交一次，Qt 线程不执行 QSaveFile。
- `status.json` 的 scanner poll 结果通过单槽后台 writer 提交；退出时 writer join，避免旧快照覆盖新 generation。
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

发布包使用 `native/package.ps1`，包内包含本记录和 P11 计划；构建产物、MCP session、宿主配置和 `artifacts/` 均不提交。
