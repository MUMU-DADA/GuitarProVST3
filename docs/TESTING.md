# 测试与验证

测试使用已安装的 Guitar Pro 8.1.1.17 进程加载仓库 DLL，不复制、覆盖或安装测试宿主。测试数据、缓存、临时 DLL 和日志写入被忽略的 `.tools/` 或 `artifacts/`。

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
音频验收使用下方的电平变化门槛。`processed_blocks`、写回 hash 和历史成功标记仅供定位，不能作为声音验收条件。

## 电平变化验收

当前用户故障的验收目标是 **曲谱播放 → 音轨 VST3 的输入与输出**，适用于该路径上的所有插件。只有目标音轨的 VST3 实例通过本节输入/输出门槛，才能声明此项通过；全局链、设备输出和内部夹具的通过不替代音轨验收。`test-audio-levels.ps1` 默认选择 `track`，必须提供准确的 `-TrackKey`，可用 `-ExpectedModule` 锁定插件路径。

`test-p2-runtime.ps1`、`test-p7-mcp.ps1`（非 `-EditorOnly`）、`test-p8-track-runtime.ps1` 和 `test-p8-standalone.ps1` 自动启用 `GPVST3_DIAGNOSTIC_MODE=detailed`，在播放期间同时检测所选 VST3 链末端和最终设备回调输出。P11 的 `-RunHost` 入口继承双音轨脚本的门槛。旁路、故障回退测试会明确标注未执行声音验收。

- 默认采样窗口为 4 秒，要求至少 6 个不同且新鲜的电平样本，覆盖至少 3 秒；实例、选择请求和音频 generation 必须保持一致。
- 冷启动等待独立计时，默认最多 60 秒，可用 `-ReadyTimeoutSeconds` 指定。只有选择请求完成、目标链准备好、收到提交后的新鲜输出电平采样，才开始 4 秒窗口；等待超时区分 `preparing` 和 `waiting_for_callback`。就绪判断允许零电平进入检测，不能一直等到出现有声样本才开始验收。结果单独保存准备等待耗时。
- 测量 peak、RMS 和去除各通道直流后的 AC RMS；RMS/AC RMS 的 20% 到 80% 分位差均须达到 **3 dB**，且线性 RMS 差至少为 **0.001**，有效信号至少为 **−60 dBFS**。对 `track`/`global` 目标还要求送入该 VST3 实例的 input AC RMS 达到同一门槛并有 3 dB 变化；输入全零、静态或只有低于门槛的噪声会直接失败。分位数避免单个脉冲造成误通过。
- 明确拒绝全零、只有直流、恒定电平、微小噪声、NaN/Infinity、缺少字段、重复/过期样本、实例切换，以及有输入时持续静音或末段丢失输出。采样或连续静音超时为 750 ms；正常短暂停顿不会单独触发失败。
- 原始样本、实际插件 module/class ID/instance、阈值、输入与输出 RMS/dBFS 变化、通过/失败和原因均保存到独立 `audio-levels.json`。失败时也保留证据。

底层每约 100 ms 测量一个完整音频块，静音结果会覆盖旧值；`vst3_output_level` 对应 `IAudioProcessor::process()` 返回后的 bus，`audio_output_level` 对应 GP 和输入路由完成后的设备缓冲。序号只用于排除旧样本，不用于判断有声。采样值通过原子快照发布，音频线程不写文件、不分配存储、不等待诊断锁；常规诊断写盘频率保持不变。

单独检查正在播放的会话：启动 Guitar Pro 前设置 `$env:GPVST3_DIAGNOSTIC_MODE = 'detailed'`，启用目标插件并循环播放有明显强弱变化的测试乐句，再运行：

```powershell
./native/build.ps1 -OutputRoot .tools/native/audio-level-build
./native/test/test-audio-level-gate.ps1
./native/test/test-audio-levels.ps1 -ObservationPath 'C:/path/to/data/p2-observation.json' -Scope global
./native/test/test-audio-levels.ps1 -ObservationPath 'C:/path/to/data/p2-observation.json' -Scope track -TrackKey '<track_runtime_evidence 中的准确 track_key>'
./native/test/test-audio-levels.ps1 -ObservationPath 'C:/path/to/data/p2-observation.json' -Scope device
```

宿主必须实际加载新构建的 DLL；仅更新脚本、使用旧 JSON 会失败。可显式指定 `-DurationSeconds`、`-MinChangeDb`、`-MinRmsDbfs` 和 `-EvidencePath`，阈值会写入结果。脚本只读取本次测量期间的新电平，不向用户工程注入测试信号。恒定音量、强限制器压平动态、长休止或已结束的播放可能不满足此测试信号门槛，应使用合适的测试乐句重新测量，不应以处理块增长替代。

该检查证明所选输出端在测量窗口内存在明确电平变化；它不是任意效果器音色正确、启停因果关系或扬声器声学输出的证明。`test-audio-level-gate.ps1` 覆盖错误证据的拒绝逻辑；`p2_audio_adapter_test` 和 `p8_runtime_test` 验证实际 bus 测量与先有声后全零时电平更新。

## P11 验证

P11 套件使用本机 Visual Studio/Qt 夹具和已安装的 MCP 服务，不使用 computer use。maintenance fixture 验证有界 latest-slot observation writer；P11 UI fixture 编译并运行 About/侧栏/DPI 回归；真实宿主选项验证 scanner、selection worker、track/global 绑定和两轨 writeback。

```powershell
./native/build.ps1 -OutputRoot .tools/native/p11-build
./native/test/test-p11.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64
./native/test/test-p11.ps1 -RunHost -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 `
  -PluginPath .tools/native/p11-build/plugins/imageformats/guitarpro_vst3_autoload.dll `
  -McpRoot C:/Users/mumu/source/GuitarProMCP -Vst3Root 'ParametricOD.vst3;Gateway.vst3'
./native/test/test-p6-package.ps1 -PluginPath .tools/native/p11-build/plugins/imageformats/guitarpro_vst3_autoload.dll
```

稳定期不再运行固定 track/observation timer；`status.json` 与 `p2-observation.json` 的 generation、sample mode、计数和时间字段用于后续固定设备 A/B 采样。真实宿主性能门槛需要单独保存 CPU/P95 原始数值，不能由 fixture PASS 代替。
