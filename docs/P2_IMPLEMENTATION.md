# P2 实现记录：GP 音频适配和实时接入

P2 已完成内部音频适配、VST3 实际块处理，以及针对 Guitar Pro 8.1.1.17 的受控实时处理路径。私有 ABI 入口仍由宿主哈希、导出存在性和函数 prologue 三重门控；默认不安装 hook，也不加载运行时效果器。

## 已实现

- `native/modules/audio_adapter.h/.cpp`
  - `BlockView` 描述输入、GP 生成音频、输出通道、帧数、采样率和 block size。
  - `PlanarBuffer::prepare()` 在控制/工作线程分配 scratch；`audio::process()` 路径只复用既有容量。
  - `copyToPlanar()` / `copyFromPlanar()` 完成 GP 通道指针与 VST3 planar `float32` 的复制，支持总旁路。
  - `audio::process()` 组装单 input/output bus 的 `ProcessData` 并调用 `IAudioProcessor::process()`。
- `native/modules/vst3_host.cpp`
  - 在工作线程扫描已安装 VST3 bundle，完成组件初始化、`setupProcessing`、激活、state/bypass 探针和 32 帧静音 `process()` 探针。
- `native/modules/gp_hook.h/.cpp`
  - 只对锁定的 Guitar Pro 8.1.1.17 文件哈希解析 `Master::process`、`EffectsChain::processDSP` 和 `AMAudio` 缓冲访问导出。
  - `GPVST3_ENABLE_P2_HOOK=1` 且 prologue 匹配时安装 x64 入口观测 hook，记录调用次数、线程 ID、帧数、通道数、采样率和缓冲哈希变化；同时按锁定 RVA 安装 `AMAudio` 的 `PortAudioAudioLayerImpl::Impl::streamCallback` hook。
  - 额外设置 `GPVST3_ENABLE_P2_EFFECT=1` 时，在原始 `Master::process` 调用后复用已预创建的 `ParametricOD` VST3 processor：GP `AudioBuffer` → planar `float32` → VST3 `process()` → 原 GP 通道缓冲。
  - 运行时 processor 就绪时状态中的 `observation_only=false`；只启用 hook 时仍为 `true`。
- `native/test/test-p2-runtime.ps1`
  - 使用旁项目的原生 MCP 驱动，通过[原软件免安装入口](P0_IMPLEMENTATION.md#原软件免安装测试2026-09-10)启动 Guitar Pro，打开独立的最小 RSE 测试曲谱并播放，读取 `p2-observation.json` 验证真实回调和处理结果。

## 验证

```powershell
./native/build.ps1
./native/test/test-p2.ps1
```

`test-p2.ps1` 会先执行原软件免安装启动回归，再调用 `native/test/test-p2-runtime.ps1` 完成真实播放处理验证；也可以单独运行后者复查运行时证据。

迁移前的 `test-p2-runtime.ps1` 历史证据（写入被忽略的 `artifacts/` 目录，计数仅对应当时的运行）：

- Guitar Pro 8.1.1.17 / Windows x64，隔离副本实际播放状态为 `playing=true`。
- `Master::process` 调用 49 次，快照块为 44100 Hz、2021 帧、2 声道；宿主缓冲哈希在调用后变化。
- `EffectsChain::processDSP` 调用 49 次。
- `PortAudioAudioLayerImpl::Impl::streamCallback` 调用 162 次，快照帧数为 1024；状态快照记录首个发生变化的输出缓冲哈希对，`audio_output_writeback_observed=true`。
- `ParametricOD` processor 初始化成功并处理 49 个块，`runtime_buffer_write_observed=true`。
- `effects_chain_inside_master=false`，两个入口的首调用线程 ID 不同，且 `cross_thread_observed=true`；因此没有把它们写成同一调用栈或固定顺序。

## 宿主受限边界

- 已证明的是 `Master::process` 返回后的 GP 缓冲可被 VST3 处理并写回，且锁定宿主版本的 `PortAudio` 最终输出回调已实际调用并发生输出缓冲写回；扬声器听感、设备切换和最终声学结果仍未验收。
- `EffectsChain::processDSP` 的调用线程与 `Master::process` 不同；当前只观测到入口序列和指针快照，`IAudioBuffer` 的完整对象所有权及跨线程同步协议仍未确认。
- `GPVST3_ENABLE_P2_EFFECT` 仍用于隔离验证；P5 面板的旁路动作已连接到实时链，但完整插件启用/重建、外部吉他 capture、设备切换、暂停/循环和采样率重配仍未完成真实宿主回归。
- 未通过宿主哈希或 prologue 检查时，hook 和运行时效果器保持关闭，插件继续默认旁路。
