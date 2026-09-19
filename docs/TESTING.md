# 测试与验证

测试使用已安装的 Guitar Pro 8.1.1.17 进程加载仓库 DLL，不复制、覆盖或安装测试宿主。测试数据、缓存、临时 DLL 和日志写入被忽略的 `.tools/` 或 `artifacts/`。

## P13 输入监听验证

普通构建包含独立 input 链、listener 分流、共享 SRC/ring 排空与 ASIO 生命周期保护。实验构建另外提供短样本、连续 PCM、逐样本共存观察和有界计时；两者必须分别验证，实验 DLL 不可发布。构建前检查 `git status`；真实宿主测试串行运行，collector 只操作自身创建的 Guitar Pro 进程，已有用户进程时拒绝启动。

### 离线专项

```powershell
./native/test/test-p4-router.ps1
./native/test/test-p13-drain.ps1
./native/test/test-p13-native-src.ps1
./native/test/test-p13-callback-split.ps1
./native/test/test-p13-input-exchange.ps1
./native/test/test-p13-input-state.ps1
./native/test/test-p13-input-runtime.ps1
./native/test/test-p13-input-ui.ps1
./native/test/test-p13-asio-lifecycle.ps1
./native/test/test-p13-minhook-strict.ps1
./native/test/test-p13-probe.ps1
./native/test/test-p13-drain-probe.ps1
./native/test/test-p13-pcm-probe.ps1
./native/test/test-p13-timing-probe.ps1
python -m unittest discover -s native/test -p 'test_analyze_p13_*.py'
```

Python 分析器需要 NumPy。各 PowerShell 入口支持独立 `-OutputRoot`，Qt 相关入口可传 `-QtDir`。这些专项覆盖 overlay/旧路由、可变帧、容量和别名、故障整块回退、排空计数、并发发布、state/editor 隔离、stream 生命周期、严格 hook 安装回滚和观测完整性；它们不等同于真实宿主音频验证。

`test-p13-native-src.ps1` 默认读取安装目录的 AMAudio.dll，也可传 `-AudioDll`；仅接受已锁定的 GP 8.1.1.17 hash。它在独立进程调用真实 SRC，不启动 Guitar Pro 或 ASIO 驱动，比较不同旧输入在排空门限后的样本。`test-p13-callback-split.ps1` 直接执行生产 callback 入口，覆盖 44100/48000/88200/96000/176400/192000 Hz × 32/64/128/256/512/1024/2048/4096/8192 帧 × 三种声道映射，以及指针边界、时间戳、空 capture、Start 期间未验证 rate 和原函数提前返回；原宿主处理函数由夹具替代。两项不能视为硬件矩阵验收。

真实第三方插件的参数 mailbox 与输入 DSP 计时使用以下可选专项，需要本机已安装且可加载的对应 VST3：

```powershell
./native/test/test-p13-neural-runtime.ps1 -PluginPath 'C:/Program Files/Common Files/VST3/Neural DSP/Archetype Mateus Asato.vst3' -SampleRate 192000 -Frames 64 -Seconds 8 -OutputRoot .tools/native/p13-neural-runtime-test
```

该专项使用合成拨弦输入，不打开设备；比较准备容量与 FTZ 配置，记录有限性、峰值、RMS、P50/P95/P99/max 和超预算块，并验证参数并发发布最终交付。计时范围仅为生产 input router 和 VST3，运行不按实时节奏，不能替代完整 GP/RSE callback 或用户硬件爆音验收。0.10.1 修订的离线对照及仍未完成的真实宿主门禁见 [P13 实现记录](P13_IMPLEMENTATION.md)。

### 普通构建与三链宿主矩阵

```powershell
./native/build.ps1 -OutputRoot .tools/native/p13-production
./native/test/build-p7-gain-fixture.ps1 -OutputRoot .tools/native/p13-gain-fixture
./native/test/collect-p13-host.ps1 -ProductionRuntime -PluginPath .tools/native/p13-production/plugins/imageformats/guitarpro_vst3_autoload.dll -InputOverlayFixture '.tools/native/p13-gain-fixture/P7 Gain Fixture.vst3' -EnableNativeListener -RseVst3 -InputOverlaySwitches 5 -CaptureSeconds 15
./native/test/collect-p13-host.ps1 -ProductionRuntime -PluginPath .tools/native/p13-production/plugins/imageformats/guitarpro_vst3_autoload.dll -InputOverlayFixture '.tools/native/p13-gain-fixture/P7 Gain Fixture.vst3' -EnableNativeListener -RseVst3 -ScopeMatrix -CaptureSeconds 5
./native/test/collect-p13-host.ps1 -ProductionRuntime -PluginPath .tools/native/p13-production/plugins/imageformats/guitarpro_vst3_autoload.dll -InputOverlayFixture '.tools/native/p13-gain-fixture/P7 Gain Fixture.vst3' -EnableNativeListener -DeviceMatrix -CaptureSeconds 5
```

`-RseVst3` 检查 input、track、global 均实际处理；`-ScopeMatrix` 使用三个独立 fixture editor 设置不同 gain，检查启停、切谱，以及清空 input 后 `active`、`dry_monitoring=true` 和重新启用恢复。`-DeviceMatrix` 请求 32～8192 帧并保存/恢复设备配置，区分成功切流、宿主拒绝后恢复、未枚举的 `host_choice_unavailable`，以及 Standard→ASIO 恢复；4096/8192 成功时还核对驱动实际 buffer 与 2048 帧处理段。拒绝或未枚举的 buffer 不算运行证据。不要把切流期间的配置拒绝计数误写为稳态插件错误。

真实 Neural 可将 `-InputOverlayFixture` 指向已安装的模块文件，省略依赖 gain fixture 的 `-ScopeMatrix` 和逐样本 unity 对照；`-MonitoringOnly -EnableNativeListener` 用于只监听、不播放曲谱的采集，不能与 `-RseVst3` 合用。冷加载必须先等待插件实际准备完成，再通过原生 action 开启本次测试监听；固定等待时长不能代替准备状态。采集期间原生输入被撤销则该段不能当作持续处理证据。

仍需用上述普通 DLL 运行 `test-p8-track-runtime.ps1 -CheckLifecycle`，覆盖音轨交换/增删/撤销/另存/重开/重启。该入口的 `-Vst3Root` 传完整绝对路径，避免重启子进程后相对路径失效。无 input 的 P8 回归只能证明旧链路，没有同时运行 input 时不能用于三链共存结论。

### 实验观察与计时

```powershell
./native/build.ps1 -EnableP13Probe -OutputRoot .tools/native/p13-probe
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/test/analyze-p13-probe.ps1 -CollectionPath artifacts/<本次目录>/collection.json
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll -InputOverlayFixture '.tools/native/p13-gain-fixture/P7 Gain Fixture.vst3' -EnableNativeListener -RseVst3 -StreamLifecycleProbe -RestartAsioStream -OverlayCoexistenceProbe -InputOverlaySwitches 5 -CaptureSeconds 15 -ProbeDelayMilliseconds 20000
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll -InputOverlayFixture '.tools/native/p13-gain-fixture/P7 Gain Fixture.vst3' -EnableNativeListener -RseVst3 -StreamLifecycleProbe -RestartAsioStream -InputOverlaySwitches 5 -CaptureSeconds 120 -ProbeDelayMilliseconds 20000
```

普通采集默认不改设备或替换输入。显式 `-RestartAsioStream`、`-DeviceMatrix` 和回环测量会在测试宿主中调整配置，并在退出前恢复、读回确认。`-NativeEffectsMatrix` 可与 RSE/input 测试组合，开启原生输入效果链并采集实际 UI 电平，结束时恢复原状态；检查 `native_effects` 和对应音频观察，不把有限的内部 peak 字段等同于电平动态验收。

普通构建的 `-NativeListenerSwitches -EnableNativeListener -RseVst3` 验证原生 Line-In 关闭后为 `waiting_for_input`、输入处理计数停止且 native suppression=false，再关闭/开启 overlay 不会覆盖用户当前 native 选择。独立冷启动专项 `test-p13-input-startup.ps1 -PluginPath <普通DLL> -FixturePath <P7 Gain Fixture.vst3>` 使用隔离 sidecar：两次保存低延迟偏好的冷启动必须在 LINE-IN 关闭时零输入处理，随后显式切换宿主输入验证跟随启停；显式保存模式 Off 后第三次启动，即使开启 LINE-IN 也保持零 overlay 处理。专项还核对参数/增益保存、原生监听状态恢复和正常退出，不修改设备配置。若宿主启动时 LINE-IN 已开启，不得把手动关闭后的结果冒充冷启动 Off 证据。input runtime 专项另通过真实 VST3 的 `kLatencyChanged` 通知验证串行延迟求和及 UI observer 更新。

`-OverlayCoexistenceProbe` 要求 unity gain 的 input fixture；逐样本比较最终 output 与本块原 output 加 capture 贡献，并比较送入 output SRC 的样本与 RSE unit 求和，另核对 drain Ready 子窗口。更换非 unity 插件后不能复用同一预期公式。有界长计时以 `data/p13-runtime-final.json` 中的实际起止、`quiesced/coherent`、admitted/completed、身份、预算及通知计数为准；`-CaptureSeconds 120` 不代表已采足 120 秒计时。P50/P95 为 histogram 桶上界，max 为实测值；驱动未发送 overload 通知不证明不存在所有物理 xrun。

在共存命令中去掉 `-InputOverlaySwitches`，增加 `-OverlayTransitionProbe -NativeEffectsMatrix -NativeTailProbe -InspectAudioUnits`，可验证首个抑制块至真正 Ready 的切换以及 native pre-gain 归零后的混响残留。观测器只读生产路由并与独立计数逐块比较，原生增益和效果链 UI 在 finally 中恢复。`-RapidInputSwitches 5` 则用于普通构建的播放中立即取消，记录过渡态并等待实际 Off/旁通恢复，不以过期诊断文件判断失败。切谱测试须等待新 transport/count-in 和 track 处理开始。

### 物理监听延迟

此专项需要持续可识别的输入 1 音源，以及 Analog Out 2→Analog In 2 的物理线。本 collector 的测量模式将 input 1 复制为监听声源，input 2 仅用于采集返回；接线、通道 selector、实际 rate/generation 和配置恢复必须全部核对。依次采集，不能并行启动两个宿主：

```powershell
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll -PcmProbe -StreamLifecycleProbe -RestartAsioStream -EnableNativeListener -MonitorLatency native
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll -PcmProbe -StreamLifecycleProbe -RestartAsioStream -EnableNativeListener -MonitorLatency overlay -InputOverlayFixture '.tools/native/p13-gain-fixture/P7 Gain Fixture.vst3'
python native/test/analyze-p13-monitor.py artifacts/<native目录> artifacts/<overlay目录> --output artifacts/p13-monitor-delay-comparison.json
```

结果口径为同步 input 1 参考到 DAC、物理线、ADC、input 2 的监听返回，包含软件监听路径；`analyze-p13-pcm.py` 的设备 output→input 回环估计是另一种口径。信号太弱、停顿、相关峰歧义、分段 lag 不稳定、身份/配置不同或恢复不完整都不能作为延迟改善证据。空白曲谱延迟专项不证明 RSE 共存；削波记录不能用于未削波保真声明。

### 发布门禁

```powershell
./native/test/test-p6-package.ps1 -PluginPath .tools/native/p13-production/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/package.ps1 -Version 0.10.1 -PluginPath .tools/native/p13-production/plugins/imageformats/guitarpro_vst3_autoload.dll
git diff --check
```

还需核对普通 DLL 不含实验 marker/环境入口，实验 DLL 被打包器拒绝且未创建 staging，以及最终发布 DLL 的真实宿主回归、安装归属收据和卸载安全。collector 的 `collected_unvalidated` 和 analyzer 的 `p13_acceptance=false` 是刻意保留的证据边界：单次工具运行不代替完整门禁。驱动实际 rate、GP 请求 rate、driver block、callback frames、CPU 耗时、插件延迟与物理监听延迟分别记录；实际结果和剩余限制见 [P13 实现记录](P13_IMPLEMENTATION.md)。

## 构建

```powershell
./native/build.ps1 -OutputRoot .tools/native/p8-track-build
```

需要指定 Qt 时使用 `-QtDir C:/path/to/Qt/5.15.x/msvc2019_64`。生产构建不包含测试用的 `-ForceNativeAudioBindings` 覆盖。

## 常用回归

```powershell
./native/test/test-p0.ps1
./native/test/test-p6.ps1 -SkipRuntime
./native/test/test-p8.ps1 -OutputRoot .tools/native/p8-delivery-suite
./native/test/test-p8-recognition-host.ps1
./native/test/test-p8-order.ps1 -P4Route input_insert
./native/test/test-p8-order.ps1 -P4Route bus_mix
./native/test/test-p8-runtime.ps1 -PluginPath .tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll -ExternalEditorPlugin 'C:/Program Files/Common Files/VST3/ParametricOD.vst3'
./native/test/test-p6-package.ps1 -PluginPath .tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll
git diff --check
```

## 音轨 runtime 回归

先运行 `./native/test/build-p7-gain-fixture.ps1` 生成测试 VST3，再分别验证 MCP bridge 和独立 native collector：

```powershell
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -Vst3Root 'C:/path/to/P7 Gain Fixture.vst3' -ExpectedBindingSource mcp_context_native_registry
./native/build.ps1 -ForceNativeAudioBindings -OutputRoot .tools/native/p8-native-build
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -PluginPath .tools/native/p8-native-build/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root 'C:/path/to/P7 Gain Fixture.vst3' -ExpectedBindingSource native_document_registry
```

## 证据边界

完整宿主回归需要匹配版本的 Guitar Pro、桌面会话和可用音频设备；未运行的设备矩阵或听感项目不能由夹具 PASS 代替。提交前不要将 `artifacts/`、`.tools/`、测试 VST3、缓存或安装包纳入提交。

## P9 验证

P9 夹具验证不使用 computer use；宿主流程使用已安装的 MCP bridge/`host-session.ps1`，并将运行数据写入独立 artifacts 目录。

```powershell
./native/test/test-p9-switch.ps1
./native/test/test-p9-ui.ps1
./native/test/test-p9.ps1
```

状态压缩和启动开关使用独立临时目录验证；完整宿主测试中，MCP bridge 的诊断值为 `mcp_context_native_registry`，表示 bridge 提供选中音轨上下文、native registry 提供可处理 EffectsChain。

切换专项记录 UI 请求确认、控制 worker 合并、准备/交接/reader drain 耗时、ramp 和音频块连续性；UI 专项覆盖 About 工具栏按钮幂等、窗口复用、窄侧栏/DPI 和扫描错误不出现在可见文本或 tooltip。`test-p9.ps1` 串联两个专项并保留 evidence。真实 Guitar Pro 听感和设备回归仍须单独记录。

## P10 验证

P10 激活夹具验证下一 callback 旁路、warm/cold slot 首个处理块和快速切换连续性；editor 回归使用生产 RuntimeEffect、真实 Qt `WA_NativeWindow` 子 HWND，并通过已安装 MCP bridge 驱动 Guitar Pro 的双击/上下文 editor、关闭和重开流程。整个流程不使用 computer use。

```powershell
./native/test/test-p10-activation.ps1 -OutputRoot .tools/native/p10-activation
./native/test/test-p10-editor.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 -PluginPath .tools/native/p10-build/plugins/imageformats/guitarpro_vst3_autoload.dll -McpRoot C:/Users/mumu/source/GuitarProMCP -Vst3Root 'ParametricOD.vst3;Gateway.vst3'
./native/test/test-p10.ps1 -RunHost -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 -PluginPath .tools/native/p10-build/plugins/imageformats/guitarpro_vst3_autoload.dll
```

状态文件中的 `selection_*`、`audio_generation`、`chain_*first_processed*` 和 `editor_stage/editor_result_code` 是 P10 的结构化证据入口。

勾选冷加载的响应性专项可使用下面的命令。`-CheckSelectionResponsive` 在初始化期间持续查询 Qt，保留每次查询耗时和目标插件行背景进度带的可见状态；要求加载期间可见、完成后隐藏。查询按进度控件的父行名称匹配，避免把其他插件或启动扫描动画误认为本次加载。`-EditorOnly` 同时检查 editor 实际关闭后重开，避免只凭 close 请求返回判定成功。快速插件的加载可能不足以采到两次进度，因此本项使用冷加载较慢的 Neural DSP。

```powershell
./native/test/test-p7-mcp.ps1 -EditorOnly -CheckSelectionResponsive -HookMode default -PluginPath .tools/native/checkbox-release2/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root 'Neural DSP/Archetype Mateus Asato.vst3;Gateway.vst3'
```

## P11 验证

P11 专项不使用 computer use；维护 writer、dirty 合并、scanner poll、P9 UI 和 P10 activation 由 `test-p11.ps1` 串联。真实宿主使用已安装 MCP bridge 与 `test-p8-track-runtime.ps1`：

```powershell
./native/test/test-p11.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 -OutputRoot .tools/native/p11-suite
./native/test/test-p8-track-runtime.ps1 -PluginPath .tools/native/p11-build/plugins/imageformats/guitarpro_vst3_autoload.dll -ExpectedBindingSource mcp_context_native_registry -HookMode enabled
```

P11 的静态门禁确认没有旧的 250/500 ms 维护路径、`musician->updateAll()` 或 `snapshot()` 音频副作用；真实宿主证据保留在 `artifacts/`，构建产物和宿主副本不提交。

## P12 验证

P12 使用生产 DLL 和 P7 Gain Fixture 验证按需运行时、selection/binding generation、无曲谱 metadata-only、stale scope 拒绝、原子旁路、warm-cache 上限及失败隔离：

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 -OutputRoot .tools/native/p12-build
./native/test/build-p7-gain-fixture.ps1 -OutputRoot .tools/native/p12-gain-fixture
./native/test/test-p12-lazy-startup.ps1 -PluginPath .tools/native/p12-build/plugins/imageformats/guitarpro_vst3_autoload.dll -OutputRoot .tools/native/p12-lazy-startup
./native/test/test-p12.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 -PluginPath .tools/native/p12-build/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root 'C:/path/to/P7 Gain Fixture.vst3'
./native/test/test-p8-runtime.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 -PluginPath .tools/native/p12-build/plugins/imageformats/guitarpro_vst3_autoload.dll -OutputRoot .tools/native/p12-runtime
```

宿主回归仍需匹配 Guitar Pro 8.1.1.17、MCP bridge 和音频设备；未运行的真实 cursor hook 计数、ASIO/WASAPI 听感和长时 CPU A/B 不由夹具 PASS 代替。

按需准备、保留实例和 warm-cache 回归由 `test-p8-runtime.ps1` 覆盖；当前不再要求完整清单预加载。历史 `test-preload-mcp.ps1` 入口保留用于三个插件逐一启停和输入监听默认关闭验证，不把脚本名当作当前启动策略；`Vst3Module` 支持与 `ClassId` 一一对应的多个模块路径。曲谱和 sidecar 写入测试副本，原始文件不变。

```powershell
./native/test/test-preload-mcp.ps1 -ScorePath C:/path/to/score.gp -PluginPath .tools/native/preload-final-build3/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Module '.tools/native/p8-order-test/P8 Order Fixture.vst3' -ClassId @('41302010605080701122334455667788','42302010605080701122334455667788','43302010605080701122334455667788')
```
