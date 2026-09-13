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

## 运行边界

| 项目 | 说明 |
| --- | --- |
| 已验证宿主 | Guitar Pro 8.1.1.17，Windows x64 |
| 音频设备 | 使用 Guitar Pro 当前配置的设备和采样率 |
| 插件格式 | 本地 VST3 bundle（由项目内嵌 host 扫描和加载） |
| 其他 Guitar Pro 版本 | 默认由版本门控拒绝实时接入并保持旁路 |
| 未完成矩阵 | 不同 ASIO/WASAPI 设备、真实扬声器听感、capture 监听/反馈、其他 GP 版本 |
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
./native/build.ps1 -OutputRoot .tools/native/p8-track-build
```

指定 Qt SDK：

```powershell
./native/build.ps1 `
  -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 `
  -OutputRoot .tools/native/p8-track-build
```

`-ForceNativeAudioBindings` 仅用于 native collector 测试构建，不用于发布 DLL。

## 安装与使用

发布包安装和用户操作见 [安装与使用](docs/INSTALL.md)。简要流程如下：

1. 关闭 Guitar Pro。
2. 解压发布包。
3. 双击 `Install.cmd`，或在 PowerShell 中运行 `./install.ps1 -Elevate -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'`。
4. 启动 Guitar Pro，在音源区域打开 `VST3`，识别完成的插件会进入可用列表。

`Install.cmd` 会在发现没有收据的旧同名 DLL 时先把旧文件备份到 `Plugins/guitarpro-vst3-backups`，再完成安装。

音轨链和全局链彼此独立，已启用插件按列表顺序处理，可拖动或使用 `Alt+Up` / `Alt+Down` 调整顺序。双击插件名称打开原生 GUI；关闭 GUI 不会停止效果处理。插件详情中可打开配置并设置“启动时启用插件”，修改在下次启动 Guitar Pro 时生效。

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
- [测试与验证](docs/TESTING.md)
- [免责声明](DISCLAIMER.md)
- [VST3 SDK 许可](third_party/vst3sdk/LICENSE.txt)

## 免责声明

本项目是独立的社区项目，与 Arobas Music、Guitar Pro、Steinberg 或任何第三方 VST3 插件作者没有隶属、授权或认可关系。项目通过私有接口和进程内 hook 工作，可能因软件更新、系统环境、音频设备或第三方插件差异而失效、旁路、产生噪声、崩溃或导致未保存状态丢失。

使用者应自行确认 Guitar Pro、第三方插件和相关音频素材的授权，并在使用前备份工程和配置。请勿在未经授权的商业、演出或关键生产环境中依赖本项目。使用本项目造成的任何数据、系统、音频设备、工程或其他损失由使用者自行承担；详见 [DISCLAIMER.md](DISCLAIMER.md)。

## 许可证

本项目代码按 [MIT License](LICENSE) 发布。仓库中的第三方代码和插件仍受其各自许可证约束。
