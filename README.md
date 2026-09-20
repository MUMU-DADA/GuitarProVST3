# GuitarProVST3

[![License](https://img.shields.io/github/license/MUMU-DADA/GuitarProVST3)](LICENSE)
[![Latest Release](https://img.shields.io/github/v/release/MUMU-DADA/GuitarProVST3?display_name=tag)](https://github.com/MUMU-DADA/GuitarProVST3/releases)

GuitarProVST3 是一个由 Guitar Pro 加载的 Windows x64 原生插件，用于在 Guitar Pro 8.1.1.17 内嵌 VST3 效果器链。它复用 Guitar Pro 的音频设备和流生命周期，不创建独立声卡流或独立宿主。

项目通过版本门控接入 Guitar Pro 的私有音频接口：只有目标版本的文件哈希和函数 prologue 校验通过时才启用实时 hook，其他版本保持旁路。

## 当前状态

P8 交付范围已在 **Guitar Pro 8.1.1.17 / Windows x64** 上完成专项验证，包括：

- 音轨级和全局 Master VST3 链；
- 后台插件识别、缓存、失败与超时队列；
- 插件 GUI、参数、链顺序和状态恢复；
- 音轨增删、重排、保存、另存、重开和进程重启恢复；
- 原生音源链共存、采样率重配置和单项故障旁路；
- 发布包、宿主哈希门控和独立运行检查。

验证证据和实现细节见 [P8 实现记录](docs/P8_IMPLEMENTATION.md)。

P9 的切换框架、紧凑侧栏、About 窗口和扫描反馈已通过夹具及 MCP 流程验证。P10 已补齐原生 editor 生命周期、首次启停旁路/提交时序和首个处理块诊断，并通过 Qt/HWND 夹具与 MCP 宿主回归；实现细节见 [P10 实现记录](docs/P10_IMPLEMENTATION.md)。

P11 已完成周期维护替换：音轨刷新、侧栏挂载、诊断写盘和 scanner poll 均按事件或任务活动触发；实现与验证见 [P11 实现记录](docs/P11_IMPLEMENTATION.md)。

P12 已改为按需实例化与有界 warm-cache，无曲谱时音轨/全局链仅维护插件元数据；P13 独立输入配置不依赖曲谱，实际监听始终跟随 Guitar Pro 的 LINE-IN 开关。实现与验证见 [P12 实现记录](docs/P12_IMPLEMENTATION.md)。

P13 的独立输入 VST3 链、低延迟监听、输入增益、状态保存和设备恢复已接入普通构建。输入监听贡献在混入 RSE 前分流，原生监听 DSP 继续维护宿主状态；共享 SRC/ring 排空后，独立输入结果叠加到保留的 GP output。音轨、全局和输入链各自拥有插件实例、参数和编辑器。

0.10.1 输入修订已实现 LINE-IN 状态门控、空链干声监听、与音轨/全局一致的插件启停和 GUI 操作流程，并重排输入窗口的监听控制、效果器列表和可折叠设备详情。保存的低延迟偏好不会自行开启 Guitar Pro 输入；未启用任何输入效果器时，按监听增益输出原始输入。

采样率与分块处理已覆盖 44100、48000、88200、96000、176400、192000 Hz，以及 32、64、128、256、512、1024、2048、4096、8192 帧。原生 SRC、排空和 callback 分块专项提供算法与离线验证证据；真实宿主的输入开关、三链隔离、冷启动和编辑器重入回归已通过。Neural DSP 已减少空闲参数同步开销并补齐混音输出范围保护；0.10.2 保留完整优化和诊断代码；用户已确认在当前 192000 Hz / 64 帧 Neural DSP 演奏场景中，开启声卡驱动安全模式后爆音消失。安全模式由声卡控制面板设置，软件不会自动更改。该反馈不代表全部硬件配置零 xrun。

0.10.0 曾在本机 Studio 2 PRO / Midiplus USB Audio ASIO、实际 192000 Hz / 64 帧（GP 内部 44100 Hz）的物理回环对照中测得监听延迟由 78.458 ms 降至 3.177 ms。这是历史版本、固定配置与测试插件的测量；0.10.1 及其他配置需要独立实测。完整证据与范围见 [P13 实现记录](docs/P13_IMPLEMENTATION.md)，要求见 [P13 计划](docs/P13_LOW_LATENCY_ASIO_INPUT_PLAN.md)。

## 运行边界

| 项目 | 说明 |
| --- | --- |
| 已验证宿主 | Guitar Pro 8.1.1.17，Windows x64 |
| 音频设备 | 使用 Guitar Pro 当前配置的设备和采样率 |
| 低延迟输入 | 跟随 Guitar Pro LINE-IN；已实现上述六种设备采样率与九档 buffer，运行时仍核对 ASIO 流与 GP 44100 Hz 拓扑；大 buffer 按不超过 2048 帧分块处理 |
| 插件格式 | 本地 VST3 bundle（由项目内嵌 host 扫描和加载） |
| 其他 Guitar Pro 版本 | 默认由版本门控拒绝实时接入并保持旁路 |
| 未完成矩阵 | 六种采样率/九档 buffer 的完整硬件矩阵、第三方插件全矩阵；WASAPI 与其他 GP 版本也未完成验收。通用控制接口回滚的 buffer 请求及未枚举的 8192 不证明驱动拒绝，也不算硬件通过 |
| 崩溃隔离 | 第三方插件进程内崩溃隔离尚未实现；识别超时只丢弃迟到结果 |

## 构建

开发环境：

- Windows x64；
- Visual Studio C++ x64 build tools；
- Qt 5.15.x MSVC x64；
- PowerShell 5.1 或 PowerShell 7+；
- 仓库内的 VST3 SDK 接口代码。

默认构建命令：

```powershell
./native/build.ps1 -OutputRoot .tools/native/release-build
```

指定 Qt SDK：

```powershell
./native/build.ps1 `
  -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 `
  -OutputRoot .tools/native/release-build
```

普通构建包含 MinHook 和 ASIO 生命周期保护。`-ForceNativeAudioBindings` 仅用于 native collector 测试；`-EnableP13Probe` 仅用于有界音频观测，实验 DLL 会被发布打包器拒绝。

## 安装与使用

发布包安装和用户操作见 [安装与使用](docs/INSTALL.md)。简要流程如下：

1. 关闭 Guitar Pro。
2. 解压发布包。
3. 双击 `Install.cmd`，或在 PowerShell 中运行 `./install.ps1 -Elevate -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'`。
4. 启动 Guitar Pro，在音源区域打开 `VST3`，识别完成的插件会进入可用列表。

`Install.cmd` 会在发现没有收据的旧同名 DLL 时先把旧文件备份到 `Plugins/guitarpro-vst3-backups`，再完成安装。

音轨链和全局链彼此独立，已启用插件按列表顺序处理，可拖动或使用 `Alt+Up` / `Alt+Down` 调整顺序。双击插件名称打开原生 GUI；关闭 GUI 不会停止效果处理。插件详情中可打开配置并设置“启动时启用插件”，修改在下次启动 Guitar Pro 时生效。

实时输入使用音源区域的“输入 VST3”：在 Guitar Pro 中开启 LINE-IN，再打开“低延迟输入监听”。可按需勾选输入插件；全部停用时保持原声直通并应用监听增益。状态显示“正在监听 · 原声直通”或“正在监听 · 输入效果器已生效”表示切换完成，准备和排空期间可能有监听空隙。关闭 LINE-IN 会停止输入监听，保存的低延迟设置不会将它重新打开。具体设备条件、插件操作和恢复行为见 [安装与使用](docs/INSTALL.md)。

## 验证

常用回归命令：

```powershell
./native/test/test-p0.ps1
./native/test/test-p8.ps1 -OutputRoot .tools/native/p8-delivery-suite
./native/test/test-p8-order.ps1 -P4Route input_insert
./native/test/test-p8-order.ps1 -P4Route bus_mix
git diff --check
```

完整测试入口、真实宿主要求和证据边界见 [测试与验证](docs/TESTING.md)。真实 Guitar Pro 宿主回归依赖本机已安装的精确版本和可用音频设备，不能由夹具测试替代。

## 文档

- [安装与使用](docs/INSTALL.md)
- [实时实现总览](docs/REALTIME_IMPLEMENTATION_PLAN.md)
- [运行逻辑与介入逻辑总图](docs/PLUGIN_RUNTIME_AND_INTERVENTION.md)
- [P8 范围与验收](docs/P8_TRACK_GLOBAL_VST3_PLAN.md)
- [P8 实现记录](docs/P8_IMPLEMENTATION.md)
- [P9 体验与稳定性计划](docs/P9_AUDIO_SWITCH_UI_PLAN.md)
- [P9 实现记录](docs/P9_IMPLEMENTATION.md)
- [P10 editor/音频生效优化计划](docs/P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)
- [P10 实现记录](docs/P10_IMPLEMENTATION.md)
- [P11 UI 性能计划](docs/P11_UI_PERFORMANCE_PLAN.md)
- [P11 实现记录](docs/P11_IMPLEMENTATION.md)
- [P13 ASIO 输入低延迟监听计划](docs/P13_LOW_LATENCY_ASIO_INPUT_PLAN.md)
- [P13 实现记录](docs/P13_IMPLEMENTATION.md)
- [测试与验证](docs/TESTING.md)
- [免责声明](DISCLAIMER.md)
- [VST3 SDK 许可](third_party/vst3sdk/LICENSE.txt)
- [MinHook 许可](native/third_party/minhook/LICENSE.txt)

## 免责声明

本项目是独立的社区项目，与 Arobas Music、Guitar Pro、Steinberg 或任何第三方 VST3 插件作者没有隶属、授权或认可关系。项目通过私有接口和进程内 hook 工作，可能因软件更新、系统环境、音频设备或第三方插件差异而失效、旁路、产生噪声、崩溃或导致未保存状态丢失。

使用者应自行确认 Guitar Pro、第三方插件和相关音频素材的授权，并在使用前备份工程和配置。请勿在未经授权的商业、演出或关键生产环境中依赖本项目。使用本项目造成的任何数据、系统、音频设备、工程或其他损失由使用者自行承担；详见 [DISCLAIMER.md](DISCLAIMER.md)。

## 许可证

本项目代码按 [MIT License](LICENSE) 发布。仓库中的第三方代码和插件仍受其各自许可证约束。
