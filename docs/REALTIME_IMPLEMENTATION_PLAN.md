# GuitarProVST3 实时实现计划

## 目标和交付边界

目标是在 Windows x64 的 Guitar Pro 8.1.1.17 中提供实时 VST3 效果器链。

最终交付形态是一个由 Guitar Pro 加载的 Qt/C++ 插件 DLL，以及运行所需的资源和配置文件。本项目的运行代码全部位于 GuitarPro.exe 进程内：

```text
GuitarPro.exe
  └─ GuitarPro 插件 DLL
       ├─ 内嵌 VST3 Host
       │    └─ 第三方 .vst3 效果器
       ├─ 音频适配层
       ├─ GP 实时处理接入
       └─ Qt 效果器链界面
```

GP 继续负责音频设备、输入输出、采样率和流生命周期。插件 DLL 负责加载和管理 VST3、处理中间音频缓冲，以及提供效果器链 UI。产品不依赖独立运行的音频应用、独立服务或虚拟声卡。

## 实现阶段

### P0：插件基础和版本锁定

**状态：已完成最小基础实现（2026-09-09）**。实现与隔离宿主证据见 [P0 实现记录](P0_IMPLEMENTATION.md)。

- 依据预研中已验证的 Qt 插件自动加载机制，建立 DLL 内部模块结构：`vst3_host`、`audio_adapter`、`gp_hook`、`effect_chain`、`qt_ui`、`state_manager`。
- 将目标宿主固定为 Guitar Pro 8.1.1.17 / Windows x64 / Qt 5.15.3。
- 记录 `GuitarPro.exe`、`GPCore.dll`、`GPRSE.dll`、`AMAudio.dll`、`AMOverloud.dll` 的 SHA-256。
- 验收：Guitar Pro 正常启动和退出，插件自动加载；插件默认旁路时不改变 GP 行为。

### P1：DLL 内嵌 VST3 Host

**状态：已完成最小 Host 实现（2026-09-09）**。实现与隔离宿主证据见 [P1 实现记录](P1_IMPLEMENTATION.md)。

- 将官方 VST3 SDK 集成到插件 DLL 的构建目标中。
- 在非实时线程完成 VST3 bundle 扫描、`GetPluginFactory` 获取、class UID 枚举和实例创建。
- 管理 `IComponent` / `IAudioProcessor` 的初始化、`setupProcessing`、`setActive`、参数、旁路、state chunk、尾音和延迟。
- 先使用本机已安装的 `ParametricOD.vst3`、`Gateway.vst3`、`NAM Rig.vst3` 做验证。
- 验收：DLL 在 Guitar Pro 进程内可以加载、初始化、旁路和释放 VST3，不产生独立进程。

### P2：GP 音频适配和实时接入

**状态：已完成适配器、VST3 实际 process 探针、哈希门控和锁定宿主版本的最终输出回调观测（2026-09-09）**。`IAudioBuffer` 跨线程 ABI、capture 所有权和听感仍为宿主受限项；实现与验证证据见 [P2 实现记录](P2_IMPLEMENTATION.md)。

- [x] 定义内部音频块结构，包含输入/生成/输出缓冲、帧数、采样率、通道数和 block size。
- [x] 提供 GP 通道指针到 VST3 planar `float32` 的复制适配，以及处理后的输出写回。
- [x] 对 `GPRSE::Master::process` 和 `EffectsChain::processDSP` 做宿主哈希/prologue 门控的运行时观测。
- [x] 在原始 `Master::process` 调用后将 GP 双声道缓冲转换为 planar `float32`，调用预创建的 `ParametricOD` 并写回原缓冲；真实播放已观测到 49 个处理块。
- [x] 锁定宿主版本中的 `PortAudioAudioLayerImpl::Impl::streamCallback` 已按 RVA/prologue 门控安装并观测到最终输出缓冲写回。
- [ ] 真实宿主中的 `IAudioBuffer` 完整所有权和跨线程同步协议仍未确认；扬声器听感与声学结果仍属于宿主受限验证。
- [x] 在已安装的 `ParametricOD.vst3`、`Gateway.vst3`、`NAM Rig.vst3` 上完成实际 `IAudioProcessor::process()` 块探针。
- [ ] GP 播放 RSE/MIDI 的最终听感仍未完成；P5 面板的旁路动作已连接到实时链，但完整插件启用/重建流程仍属于宿主受限项。

### P3：实时线程安全和链管理

**状态：已完成固定双槽链、原子切换、旁路/错误回退、处理监控和重配置矩阵实现（2026-09-09）**。实现与验证入口见 [P3 实现记录](P3_IMPLEMENTATION.md)。

- 音频线程不扫描磁盘、不创建 Qt 对象、不动态分配、不等待阻塞锁。
- 插件实例在工作线程预创建，使用预分配缓冲和原子链切换。
- 增加总旁路、处理耗时监控，以及针对可检测处理错误的旁路回退。进程内第三方插件的原生崩溃不能保证被恢复，超时监控也不能安全中断任意 `process()` 调用。
- 在 44.1/48/96 kHz 以及 64/128/256 frame block 下验证重新配置流程。

### P4：外部吉他输入

**状态：已完成 capture 适配层、输入电平监控、输入插入/总线混音路由和隔离回归（2026-09-09）**。真实 `PortAudio` capture 指针的私有回调 ABI 尚无稳定导出，真实监听和设备回归仍为宿主受限项；实现与验证证据见 [P4 实现记录](P4_IMPLEMENTATION.md)。

- [x] 新增固定预分配 capture tap，读取 `AudioLayer::inputLevel/isRunning/bufferSize` 并输出原子输入电平快照。
- [x] 实现外部吉他 `input_insert`，处理失败时直通并记录旁路/错误计数。
- [x] 实现 GP 回放与外部输入分别处理，以及混合后进入 `bus_mix` 总线效果器。
- [x] `PortAudioAudioLayerImpl::Impl::streamCallback` 的最终输出缓冲写回已在锁定宿主版本中观测。
- [ ] 该回调中的真实 capture buffer 所有权和输入通道布局仍待宿主 ABI 证据补齐。
- [ ] 真实宿主中的输入监听稳定性、反馈、设备切换、暂停/恢复听感：当前未运行，标记为宿主受限。

### P5：Qt 界面和状态保存

**状态：已完成最小 Qt 链面板、sidecar JSON 保存、缺失插件旁路和隔离验证（2026-09-09）**。实现与验证入口见 [P5 实现记录](P5_IMPLEMENTATION.md)。

- [x] 在“音源”区域旁增加“VST3 效果器链”面板；无稳定 GP 私有控件插槽时使用右侧 dock 和音源区入口。
- [x] 提供添加、删除、上移、下移、旁路、搜索和参数编辑。
- [x] 首版使用 sidecar JSON 保存 GP 曲谱标识、track/bus、插件路径、class UID、参数、state chunk 和 bypass 状态。
- [x] 插件缺失或状态恢复失败时显示为 bypass，并允许用户重新选择插件。
- [x] 插件编辑器窗口仍由 Guitar Pro 插件 DLL 创建和管理；本项目不伪造第三方编辑器。

### P6：版本门控、回归和发布

**状态：已完成版本门控、隔离/真实宿主回归入口、安装归属和可复现发布包（2026-09-09）**。实现与验证入口见 [P6 实现记录](P6_IMPLEMENTATION.md)。

- [x] 仅对已验证的宿主哈希启用私有 ABI hook；版本变化自动禁用实时模式。
- [x] 覆盖播放/停止、循环、换曲谱、保存重开、退出、插件缺失、插件异常回退和安装卸载归属。
- [x] 记录可用音频后端、采样率、通道数和 buffer size 的配置/重配置结果；WASAPI/DirectSound 独立选择、暂停接口和最终声卡回调标记为宿主受限。
- [x] 检查 `git diff --check`、构建结果、发布包文件白名单、文件哈希和敏感文件。

## 核心技术约束

- VST3 维护独立效果器链，不伪装成 GP 私有 `core::Effect` 或 `am::overloud::Effect`。
- GP 的输入输出设备逻辑保持不变，插件只接入统一音频缓冲处理位置。
- UI、文档操作和链配置变更在 Qt 主线程执行；实时音频处理只使用已准备好的对象。
- 未经哈希验证的 Guitar Pro 版本默认关闭实时 hook，避免未知 ABI 继续运行。
- 音频功能验证在加载插件的 Guitar Pro 中完成，不构建或分发独立宿主测试程序；日志只写入本地诊断目录。

## 里程碑

以下均为实施目标，尚不代表能力已实现或通过真实宿主验证。

1. **M0**：Git、许可证、计划和目录骨架完成。
2. **M1**：Guitar Pro 能自动加载 DLL，DLL 内 VST3 Host 完成插件扫描和生命周期验证。
3. **M2**：RSE/MIDI 实时播放接入一个 VST3 效果器。
4. **M3**：外部吉他输入接入和三种路由验证完成。
5. **M4**：效果器链 UI、sidecar 状态和版本门控完成。
6. **M5**：真实 Guitar Pro 回归矩阵和发布包完成。

## 依据

接入点、私有 ABI、`IAudioBuffer`、RSE 处理链和输入输出限制依据目录中的预研报告：

- [VST3 效果器链路预研报告（最终版）](../gp8-vst3-research-20260909/VST3效果器链路预研报告_最终版.md)
- [原始预研报告](../gp8-vst3-research-20260909/research_report.md)
