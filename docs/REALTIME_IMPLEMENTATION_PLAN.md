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

**状态：已完成 capture 适配层、输入电平监控、输入插入/总线混音路由、锁定宿主回调回归和隔离验证（2026-09-10）**。真实 `PortAudio` capture 指针的私有 ABI 仅在锁定版本门控下使用，所有权、真实监听和设备回归仍为宿主受限项；实现与验证证据见 [P4 实现记录](P4_IMPLEMENTATION.md)。

- [x] 新增固定预分配 capture tap，读取 `AudioLayer::inputLevel/isRunning/bufferSize` 并输出原子输入电平快照。
- [x] 实现外部吉他 `input_insert`，处理失败时直通并记录旁路/错误计数。
- [x] 实现 GP 回放与外部输入分别处理，以及混合后进入 `bus_mix` 总线效果器。
- [x] `PortAudioAudioLayerImpl::Impl::streamCallback` 的最终输出缓冲写回已在锁定宿主版本中观测。
- [x] 锁定版本的 PortAudio 参数快照已接入 capture adapter：仅接受交错 `paFloat32`，按当前输入/输出通道数转换，并在回调返回前写回。
- [ ] 该回调中的真实 capture buffer 所有权仍待宿主 ABI 证据补齐；当前只记录 borrowed pointer 的回调期地址，不保留指针。
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

## P7：VST3 同级选区、自动清单和原生 GUI（最终计划）

**定位：新需求统一放在 P7；P0–P6 原有计划和 `docs/P5_IMPLEMENTATION.md` 保持不变。**

### P7.1 目标和固定交互

P7 针对以下四项最终需求实施：

1. VST3 接入界面放在 Guitar Pro 现有效果器链选择区域的同级位置，使用该区域的父容器、布局和可见性生命周期。
2. 选区自动查询本机已经安装的 VST3 效果器并列出，不要求用户手动选择 `.vst3` 文件、填写路径或配置插件参数。
3. 每个插件只有两种用户状态：选中表示启用，未选中表示停用。取消某一项只停用该项，其他已选插件继续处理。
4. 点击任意已启用插件的名称，打开或聚焦该插件自己的图形界面；插件参数全部在其原生 GUI 内设置，项目不再制作额外的插件设置面板。

复选框负责启停，名称点击负责打开 GUI，两个点击区域分开。P7 不提供添加、删除、搜索、上移、下移、手动路径、参数表、额外旁路、保存按钮或独立 dock 菜单。多项同时启用时按清单稳定顺序串联。

### P7.2 真实宿主和构建前提

本机已安装的 Guitar Pro 本体目录为：

```text
C:\Program Files\Arobas Music\Guitar Pro 8
```

P7 直接以该目录中的以下文件作为锁定宿主检查对象，不修改原文件：

```text
GuitarPro.exe
GPCore.dll
GPRSE.dll
AMAudio.dll
AMOverloud.dll
```

执行顺序如下：

- 先对本体目录和当前已验证文件做只读清单、版本信息、SHA-256 和 PE 架构记录；需要 hook 或运行测试时复制到隔离目录，不在安装目录内写入测试文件。
- 使用本机已安装的 C++ Build Tools/MSVC x64 工具链编译和链接；构建脚本应从 Visual Studio Developer 环境或 `vswhere` 找到 `cl.exe`、`link.exe` 和 Windows SDK，不把编译器路径写死进仓库。
- 继续使用现有 `native/build.ps1`、Qt 5.15.x MSVC x64 配置和宿主哈希门控。构建成功只证明代码可编译，不证明 P7 交互或声音链路完成。
- 在真实 Guitar Pro 进程中观察现有效果器选区的 QWidget 类名、objectName、父布局、显隐通知、曲谱/轨道上下文和窗口重建；只记录可复现证据，不猜测私有对象布局。

### P7.3 同级入口和作用域接入

**状态：入口已补齐；同级布局仍宿主受限。** 入口不再启动时自动弹出，按钮会持续补回右侧“音源”区域；当前锁定版本尚未取得可重复的同级父布局插入契约。实现边界见 [P7 实现记录](P7_IMPLEMENTATION.md)。

- 找到现有效果器链选择区域的稳定父 QWidget 或稳定插入通知后，在同一父容器中增加紧凑的 VST3 选区，并复用 GP 的尺寸、字体、间距和折叠行为。
- 处理重复加载、窗口缩放、区域重建、曲谱切换和插件卸载，确保不会出现重复入口或悬空 QWidget。
- 确认该选区对应的实际处理作用域是当前轨道、总线或总输出，并让 UI 选择和音频 hook 使用同一上下文。不得显示轨道级选区却把声音未经确认地送到总输出，也不要求用户手填 Track/Bus。
- 同级位置是 P7 的硬性验收条件。若锁定版本没有稳定插入点，只记录为“宿主受限”并暂停该能力；右侧 dock 或独立窗口不算完成。

预计修改：`native/modules/qt_ui.cpp/.h`、`native/modules/bootstrap.cpp`，必要时扩展 `native/modules/gp_hook.cpp/.h` 的上下文通知。

### P7.4 自动扫描和清单模型

**状态：已实现标准目录元数据清单（2026-09-10）**。默认扫描不创建 processor，显式开发路径保留 P1 生命周期探针；证据见 [P7 实现记录](P7_IMPLEMENTATION.md)。

- 去掉当前对 `ParametricOD.vst3`、`Gateway.vst3`、`NAM Rig.vst3` 的默认名称限制。
- 工作线程递归扫描 Windows VST3 标准目录：`%ProgramFiles%/Common Files/VST3`、`%ProgramFiles(x86)%/Common Files/VST3`、`%LOCALAPPDATA%/Programs/Common/VST3`。加载 bundle 的 `GetPluginFactory`，枚举音频效果 class 的 class UID、名称、厂商、类别和模块路径。
- 按规范化模块路径 + class UID 去重；按 PE 架构和音频总线能力过滤当前 x64 宿主无法加载或无法作为效果器处理的 class。纯乐器或不兼容插件不显示为可启用效果器，并在诊断状态中记录原因。
- 宿主 UI 就绪或选区首次打开时自动扫描，目录变化在再次打开时自动检查。扫描不发生在实时线程，不为仅列清单的插件创建 processor。
- 列表只显示插件名称，重名时附厂商。新发现的插件默认未选中，不自动启用；标准目录以外的插件不作全盘发现承诺。`GPVST3_VST3_PATHS` 和 `GPVST3_VST3_ROOT` 仅作为开发/测试扫描入口，不增加用户扫描设置页。

预计修改：`native/modules/vst3_host.cpp/.h`、`qt_ui.cpp/.h`、`bootstrap.cpp`。

### P7.5 二态启用和实时链

**状态：已实现二态 sidecar/UI 语义；清单驱动多实例实时链宿主受限。** `checked = enabled`、旧 `bypass` 迁移和安全旁路已验证；P3 双槽 callback 尚未取得多实例串联证据。

- 统一语义为 `checked = enabled`，修正当前 `checked = bypass` 的相反语义。界面不暴露第三种状态；实例准备失败时将该项恢复为未选中并显示简短错误。
- 勾选后在非实时路径准备该 class 的 component、processor、controller、总线和预分配缓冲，成功后把实际实例加入当前作用域的处理链。不能只改变 JSON 或复选框就宣称启用。
- 支持两个及以上选中插件按稳定清单顺序串联；取消一项只移除该项，剩余实例和设置保持有效；全部取消时直通。
- 复用 P3 双槽和原子发布，音频线程不扫描磁盘、不创建 Qt 对象、不动态分配、不等待阻塞锁。快速连续勾选时以最后一次选择为准，确保 UI 状态和已发布链一致。
- P4 外部输入继续复用同一清单和处理链，不增加第二套插件选择或路由设置。未知宿主哈希、处理错误和不满足缓冲条件时保持安全旁路。

预计修改：`native/modules/vst3_host.cpp/.h`、`effect_chain.cpp/.h`、`gp_hook.cpp/.h`、`qt_ui.cpp/.h`。

### P7.6 原生 VST3 GUI

**状态：宿主受限。** 名称点击入口和未启用提示已实现，锁定 GP 版本的 `IPlugView`/HWND ABI、参数消息和同实例 state 回传未验证。

- 名称点击只对已启用项生效，从正在处理音频的同一实例取得 `IEditController` 和 `IPlugView`；不另建只用于显示的实例。
- 在 Qt 主线程通过 HWND 承载 `IPlugView`，处理 `IPlugFrame` 尺寸回调、DPI、焦点、重复点击聚焦、窗口关闭和 GP 退出释放。
- 补齐 component/controller 连接和 `IComponentHandler` 回调，把原生 GUI 的参数编辑消息送入 processor 的预分配消息队列，必要时处理 `restartComponent`，确保 GUI 调节真实影响输出。
- 关闭 GUI 只关闭 view，插件保持启用并继续处理；取消勾选时先取得插件 state、关闭 view，再在实时读者退出后安全停用和释放实例。
- 对没有 editor view 或不支持 Windows editor 的插件记录兼容性结果并提示原因，不伪造项目自己的参数设置页。

预计修改：`native/modules/vst3_host.cpp/.h`、`qt_ui.cpp/.h` 和参数消息进入实时处理的适配部分。

### P7.7 状态保存和升级

**状态：已实现 `enabled` 迁移和自动保存（2026-09-10）**。schema 1 保持兼容，旧 `bypass` 读取为 `enabled = !bypass`。

- 复用现有 `QSaveFile` 和 sidecar 路径，自动保存插件身份、清单顺序、`enabled` 状态以及 VST3 component/controller 提供的 opaque state。用户不直接编辑这些字段。
- 旧数据中的 `bypass` 映射为 `enabled = !bypass`；保留旧参数字段以便兼容，但不再在 UI 中显示参数表。只有实际需要时才升级 schema，并提供读取旧 schema 的迁移路径。
- 清单变更、曲谱上下文切换和宿主退出时自动保存，不增加额外保存按钮。取消勾选或关闭 GUI 不丢失插件自己的设置。
- 插件缺失、class UID 不再存在或 state 恢复失败时保持未选中并保留数据；重新安装后按相同插件身份恢复，不要求手工重新选择文件。

预计修改：`native/modules/state_manager.cpp/.h`、`vst3_host.cpp/.h`、`qt_ui.cpp/.h`。

### P7.8 验收和证据

**状态：部分验证；关键项标记宿主受限。** 新增 [P7 实现记录](P7_IMPLEMENTATION.md)、`native/test/test-p7.ps1` 和 `native/test/test-p7-ui.ps1`；同级插入、多实例实时链和原生 GUI 没有真实宿主证据，因此不标记为完整完成。

| 验收项 | 必须取得的结果 |
| --- | --- |
| 本体检查 | 对 `C:\Program Files\Arobas Music\Guitar Pro 8` 的宿主文件完成只读版本、SHA-256、PE 架构和隔离副本记录 |
| 同级位置 | VST3 选区与现有效果器选区处于同一父容器/布局；缩放、折叠、曲谱切换和窗口重建后不重复、不漂移 |
| 自动列表 | 标准目录中安装的兼容 VST3 class 均可自动出现在列表；多 class、重复路径、空目录、插件增删和扫描失败有记录 |
| 二态启用 | 选中插件产生真实处理，取消只停用该项，两个以上插件按列表顺序串联，全部取消直通 |
| 原生 GUI | 每个支持 editor 的已启用插件都能打开/聚焦；GUI 参数修改造成可观测音频变化；关闭 GUI 后效果继续生效 |
| 状态恢复 | 重启/重开恢复启用和插件 state；缺失、损坏或不兼容数据不会误启用 |
| 实时安全 | 沿用 P2/P3/P4 的采样率、block size、错误回退、输入路由和退出释放验证 |
| 界面简化 | Qt 夹具确认只保留复选框和插件名称交互，没有旧的手工选择、参数表、排序和独立 dock 入口 |
| 发布 | 构建、安装/卸载归属、宿主哈希门控、发布文件白名单、敏感文件检查和 `git diff --check` 全部通过 |

编译成功、DLL 能加载、菜单能枚举、返回 JSON 或 editor 窗口能创建，均不能单独证明 P7 完成。真实 GP 中同级位置、作用范围、勾选后的声音变化和原生 GUI 参数联动必须分别记录。

### P7.9 交付顺序和完成标准

1. **P7-A**：只读检查 Guitar Pro 本体和 C++ Build Tools，建立隔离副本及哈希基线。
2. **P7-B**：确认同级 QWidget 容器和实际音频作用域，完成同级入口原型。
3. **P7-C**：替换测试插件白名单为本机 VST3 自动扫描清单。
4. **P7-D**：完成 `checked = enabled` 的单插件、多插件、取消和全取消实时链。
5. **P7-E**：完成同一实例的 `IPlugView` 打开、聚焦、参数消息回传和 state 保存。
6. **P7-F**：在真实 Guitar Pro 运行完整回归，新增 P7 实现记录，更新发布包和能力状态。

P7 只有在 P7-B 至 P7-F 的真实宿主验收证据齐全后才标记为已实现；同级入口不可用、只能处理总输出、插件 GUI 参数未改变声音或只能依赖右侧 dock 时，分别标记为宿主受限或未实现。
