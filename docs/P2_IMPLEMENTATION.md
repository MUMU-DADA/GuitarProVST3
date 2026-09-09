# P2 实现记录：GP 音频适配和实时接入

P2 已完成可在当前证据范围内安全验证的部分：内部块结构、planar `float32` 适配、预分配 scratch、VST3 `process()` 探针和 GP 私有入口只读观测。私有 ABI 的运行时改写仍保持关闭，因为预研没有确认 `IAudioBuffer` 对象布局、调用线程、所有权和后续写回顺序。

## 已实现

- `native/modules/audio_adapter.h/.cpp`
  - `BlockView` 同时描述外部输入、GP 生成音频和输出缓冲，以及帧数、采样率、通道数和 block size。
  - `PlanarBuffer::prepare()` 在控制/工作线程分配输入和输出 planar 缓冲；`process()` 路径只复制既有内存，不改变容器容量。
  - `copyToPlanar()` / `copyFromPlanar()` 负责 GP 通道指针与 VST3 planar `float32` 之间的复制。存在 generated buffer 时优先处理生成音频，否则使用 input buffer。
  - `audio::process()` 组装单 input/output bus 的 `ProcessData`，支持总旁路并把处理结果写回输出通道。
- `native/modules/vst3_host.cpp`
  - 每个已创建并进入 processing 状态的音频组件，在工作线程使用 32 帧静音块实际调用一次 VST3 `IAudioProcessor::process()`。
  - 状态记录 `process_calls`、`process_probes_passed`、`process_probe_frames` 和失败原因；处理探针失败时不计入通过的生命周期。
- `native/modules/gp_hook.h/.cpp`
  - 仅在宿主文件哈希匹配时解析 `GPRSE.dll` 的 `Master::process`、`EffectsChain::processDSP` 和 `AMAudio.dll` 缓冲访问导出。
  - 记录模块/导出是否存在，明确返回 `observation_only`；没有写入 GP 代码，也没有在未知 ABI 上安装 trampoline。
- `native/test-p2.ps1`
  - 在隔离 Guitar Pro 副本中复用 P0/P1 启动测试，检查 planar 适配器状态、真实 VST3 process 探针和 hook 只读状态。

## 验证

```powershell
./native/build.ps1
./native/test-p2.ps1
```

当前机器的 Guitar Pro 8.1.1.17 隔离副本中，直接启动和快捷方式启动均通过。每次发现并加载 3 个 VST3 bundle，3 个音频组件的 32 帧 `process()` 探针均通过；`Master::process`、`EffectsChain::processDSP` 和音频缓冲访问导出均可解析，hook 状态为 `installed=false`、`observation_only=true`。测试证据写入被忽略的 `artifacts/` 目录。

## 宿主受限边界

- `call_observed=false`、`buffer_write_observed=false`：现有预研材料没有提供真实播放回调的线程、`IAudioBuffer` 所有权和写回顺序证据，因此没有把函数导出存在误写成运行时接入完成。
- 没有声明 GP 播放时已经听到效果；当前验证覆盖的是实际第三方 VST3 processor 的进程内块处理探针。
- 外部吉他 capture buffer、播放/暂停/循环切换、设备切换、采样率和 block size 重新配置属于后续真实宿主回归，未在本阶段伪造通过。
