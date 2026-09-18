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
- 音轨运行时不再从 sidecar 自动恢复 `enabled=true`；打开曲谱时音轨插件保持停用，必须由用户重新勾选。慢初始化完成后再次校验请求代际，已取消或被替换的旧请求会被旁路并丢弃，不会重新激活旧插件。
- 无曲谱时不创建 track/global P7 panel；曲谱上下文确认后再挂载 native section。
- 新曲谱若复用宿主 `IDocument`，其临时 UUID score key 不再触发 Save As 状态迁移；因此不会继承上一曲谱的 track enabled 意图，只有真实文件路径的 Save As 才复制原有链。
- 启动宿主文件哈希校验移到独立后台线程，ABI/Qt 相关初始化拆成多个事件回合；首次宿主曲谱上下文发现延后到首帧事件循环之后；标题工具栏（不可用时使用主窗口状态栏）显示一个可收起的小型启动进度条，校验/识别期间使用忙碌动画，后台 catalog 扫描和识别进入终态后自动隐藏。

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

勾选响应性修复（2026-09-17）：列表勾选仍使用原 selection worker，但不再把 component/controller 的 `initialize`、初始状态读取/恢复和 processing setup 整段送回 Qt。factory 创建保留在 Qt，以保持 Neural DSP GUI 对象的线程归属；编辑器挂载/关闭沿用原线程路径。worker 空闲时运行事件循环。首次 hook 安装前只发布轨道元数据，防止 context worker 持锁等待 Qt 与首次勾选相互等待。勾选期间只在本次选中的插件行背景显示透明鼠标穿透的移动进度带，worker 完成/失败后收起；面板销毁及排队 preload 完成也有收尾通知。

专项回归在真实 Guitar Pro 完成启动布局后，分别冷建两个 track/global 实例；component initialize、独立 controller initialize 和状态恢复注入 150 ms 延迟，持续采样 10 ms Qt 心跳，要求最大间隔小于 100 ms。夹具同时验证首次 hook 前的 context 不请求 Qt、状态/采样率/故障隔离和编辑器生命周期。P9 覆盖进度显示、完成隐藏及面板销毁后的隐藏，P11 调度/UI/激活套件通过（`.tools/native/checkbox-final-p11`）。生产 DLL 输出为 `.tools/native/checkbox-release2/plugins/imageformats/guitarpro_vst3_autoload.dll`。

最终 P8 证据为 `artifacts/p8-runtime-ccb42bc97ffe4e53bfaa908992d4f081`：track/global 冷准备总耗时 1816/2120 ms，各创建两个新实例；勾选请求均为 0 ms，最大 Qt 心跳间隔 12/13 ms，`runtime-test.json` 返回 0。该运行还覆盖了 preload 完成通知，未出现延迟插件调用进入 Qt 的诊断。

行背景动画调整（2026-09-18）已通过 P8/P9 三档 DPI 和 P11 回归，生产 DLL 为 `.tools/native/row-progress-final/plugins/imageformats/guitarpro_vst3_autoload.dll`。真实宿主 Neural DSP 全局勾选证据为 `artifacts/mcp-p7-89e9854e8f714f5196a91121cf24bfea`：勾选返回 8.4 ms，加载期间最长 Qt 查询 75.9 ms，目标行进度带可见并在完成后隐藏；editor 关闭后重开通过。音轨专项另使用最新构建覆盖新曲谱、停用和替换插件路径，见下方证据。

新曲谱继承隔离与音轨启停专项（2026-09-18）使用 `.tools/native/track-final-release/plugins/imageformats/guitarpro_vst3_autoload.dll` 通过：带旧 sidecar 冷启动的新曲谱初始活动音轨实例为 0，A→空 selection 后 `configured_effects=0` 且旁路增长，随后 B `configured_effects=1` 并持续处理；证据为 `artifacts/mcp-p8-track-24112b71712f4db691becbf4bc5e4835`。同一构建的空 sidecar 回归证据为 `artifacts/mcp-p8-track-0ef633c3393c432e8d1f68ade073dba4`，状态层 transient UUID 隔离单测为 `.tools/native/p8-state-final3`。

Neural DSP 音轨真实插件回归同样通过（Nolly X → 停用 → Mateus Asato）：新曲谱 baseline 活动实例为 0，selection request `2 -> 4`，停用阶段 `configured_effects=0` 且旁路块增长，Mateus Asato 阶段 `configured_effects=1`、处理块持续增长；证据为 `artifacts/mcp-p8-track-15957a6490304327b6b1f832918a2bbd`。该专项对 Neural DSP 的慢初始化保留 30 秒运行时证据等待窗口，Qt 线程仍保持事件循环可用。

真实勾选验证使用 `test-p7-mcp.ps1 -EditorOnly -CheckSelectionResponsive -HookMode default`：在整个冷加载阶段持续通过 MCP 查询 Qt 对象，检查忙碌进度可见→隐藏，并实际关闭后重开 editor。该检查区别于只测 `setChecked` 返回或只读磁盘状态；宿主安装目录由测试完整性检查保护。

最终生产 DLL 在本机 7 款 Neural DSP 上通过上述勾选与 editor 回归，进度均正常收起，宿主均自然退出（exit code 0，无强制结束）：

| 插件 | 勾选请求 ms | 加载期间最长 Qt 查询 ms | artifacts/mcp-p7 证据后缀 |
| --- | ---: | ---: | --- |
| Archetype Nolly X | 7.7 | 101.0 | `6bb7ae43b23845cda5bb496dfa1a70dc` |
| Archetype Mateus Asato | 5.8 | 48.4 | `1ca37764e7dc4a3181078a5af83add64` |
| Archetype Petrucci X | 5.7 | 50.1 | `34ffe29cb87d4edfb857d5898324195b` |
| Archetype Rabea X | 6.3 | 51.4 | `aa0a6078b26340ccbb4daac5648a0669` |
| Archetype Tim Henson X | 6.2 | 92.0 | `7950b1a4bd884ecaa08f71bfc575c2b5` |
| Morgan Amps Suite | 1.6 | 55.5 | `f7ce4ed7be504f95bb086aeef175e087` |
| Soldano SLO-100 X | 5.7 | 47.8 | `225ee4dabdcc4c339d427ef81a0580b3` |

Nolly X 另外从带 5344 字节 component state 的停用 sidecar 冷勾选，含状态恢复耗时约 9 秒；勾选返回 7.6 ms，最长 Qt 查询 49.8 ms，进度及 editor 关闭/重开通过（`artifacts/mcp-p7-17b3070509664edba53dc8e8383d3b6f`）。这证明已保存参数的重启启用路径也未把慢状态恢复送回 Qt。上述证据限于本机插件版本；factory 对象构造保留 Qt，不能据此宣称任意第三方构造函数都没有耗时，其他版本和设备听感仍需分别验收。

P8 runtime 夹具已通过以下 P12 回归：只预加载已启用 scope、stale generation 拒绝、停用旁路、异步启用、状态保留、采样率重配、失败隔离、编辑器生命周期和输入路由默认关闭。P12 集成套件已通过：`.tools/native/p12-release-suite5`。其中包含无曲谱启动、P11 调度/UI/激活、A→B→A 选择代际、双音轨实例与状态、复制/交换/撤销/保存重开/进程重启和最终宿主完整性检查。识别缓存 writer 和超时队列分别由 `p12-recognition2`、`p12-recognition-timeout5` 验证，静态 catalog 由 `p12-p7-catalog` 验证。

## 宿主边界

当前已在匹配的 Guitar Pro 8.1.1.17 + MCP 会话中观察到 `selection_hook_installed=true`、`selection_hook_gate_passed=true` 和 `selection_event_source=native_cursor_hook`；未通过 host hash/prologue gate 时保持安全旁路。其他 Guitar Pro 版本、真实 ASIO/WASAPI 听感、第三方进程级崩溃隔离和长时 CPU A/B 不在本阶段自动门禁内。
