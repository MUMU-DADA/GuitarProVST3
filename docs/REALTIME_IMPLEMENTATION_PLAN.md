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

**当前总状态：进行中（2026-09-10 更新）。** 已有自动清单、二态实时链和原生 editor 桥接基础；按最新交互要求，剩余工作是原生 GUI 独立窗口、跨重启的扫描缓存、首次扫描按钮提示及对应验收。具体顺序见 [P7.8 剩余实施计划](#p78-剩余实施计划2026-09-10)，现有实现和历史证据见 [P7 实现记录](P7_IMPLEMENTATION.md)。

### P7.1 交互和同级入口

- `gpvst3P7Panel` 与 `gpvst3SoundEffectChainButton` 在 MCP 真实宿主中确认是 `soundsContainer` 的直接子级，并沿用该区域的布局和重建生命周期。
- 面板只显示自动发现的兼容 VST3 audio effect。复选框负责启停，插件名称负责打开原生 GUI；稳定 objectName 为 `gpvst3Enabled_<classId>`、`gpvst3Editor_<classId>` 和 `gpvst3NativeEditorHost`。
- 面板重建会重新挂接入口，关闭面板后再次打开会重新创建对象；没有使用独立 dock 作为 P7 入口。
- 同级位置要求用于插件选择区。点击已启用插件名称后，其原生 GUI 应在 Guitar Pro 所属的独立非模态窗口中打开；选择列表应继续可见、可操作。

### P7.2 自动清单

**状态：异步扫描已实现；磁盘缓存和按钮提示待实现。** 工作线程递归扫描 `%ProgramFiles%/Common Files/VST3`、`%ProgramFiles(x86)%/Common Files/VST3` 和 `%LOCALAPPDATA%/Programs/Common/VST3`，按规范化 bundle 路径和 class UID 去重，过滤 instrument class。标准目录扫描使用 `rundll32` 隔离调用 `Gpvst3Scan`，每个 bundle 10 秒限时；显式 `GPVST3_VST3_PATHS`/`GPVST3_VST3_ROOT` 才进行生命周期探针。

当前 bootstrap 在启动时调用 `beginAsync()`，`g_cachedScan` 只在本次进程内复用。`QTemporaryDir` 中的 `catalog.json` 是扫描子进程的临时结果，不是下次启动可以读取的缓存。标准目录历史回归存在第三方 bundle 超时，清单数量随本次扫描结果变化，详见实现记录。

### P7.3 二态启用和实时链

**状态：已实现并通过 MCP 回归。** `checked = enabled`，旧 `bypass` 迁移为 `enabled = !bypass`。启用项在控制线程准备 component/controller/processor 和预分配 planar 缓冲，通过双槽链原子发布；两个实例按清单顺序串联，取消单项会复用其余实例，全部取消后直通。音频线程不扫描磁盘、不创建 Qt 对象、不动态分配、不持有 UI 锁。

默认启动的首次勾选会在宿主哈希/prologue 校验后启用实时接入，无需开发环境开关；显式禁用配置仍生效。含空格安装路径的自动扫描、默认启动勾选及失败回滚已补充验证，见 [正常启动后的勾选修复](P7_IMPLEMENTATION.md#正常启动后的勾选修复2026-09-10)。

### P7.4 原生 VST3 GUI

**状态：原生桥接已有实现；独立窗口待实现和验证。** 处理链持有的同一实例创建 `IPlugView`，并提供 `IPlugFrame`、component/controller `IConnectionPoint`、`IComponentHandler` 和每参数预分配 mailbox。当前 `NativeEditorWindow` 使用 `QWidget(owner, Qt::Widget)`，editor host 位于 `gpvst3P7Panel` 内，因此仍会占用选框区域。历史 MCP 的父级关系和“原生 GUI 已打开”文字只证明旧桥接路径，不能作为独立窗口验收。

### P7.5 状态保存

**状态：已实现并验证。** sidecar 保存 module/class UID、`enabled`、component state 和 controller state；取消选择、关闭 GUI、面板析构和宿主退出都会捕获 state。最近一次 MCP sidecar 保存 Gateway 151 字节、ParametricOD 52 字节 state。

新增扫描缓存只保存发现结果和文件指纹；启用状态及插件参数仍由 `effect-chain.json` 保存。缓存失效、删除或重建不得清空该文件中的插件设置。

### P7.6 作用域和宿主边界

当前 hook 的可证实位置是 master 后处理点，不能宣称已取得 Guitar Pro 轨道级或音源级作用域。真实声学听感、设备切换、外部输入监听和每个第三方插件的 editor 兼容性仍按 P2/P4 宿主受限处理。

### P7.7 验收命令和交付

```powershell
./native/build.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native
./native/test/test-p7-ui.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64
./native/test/test-p2-runtime.ps1 -PluginPath .tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/test/test-p7-mcp.ps1 -PluginPath .tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/test/test-p7-mcp.ps1 -HookMode default -StandardScan
./native/test/test-p7-mcp.ps1 -HookMode disabled
./native/test/test-p7.ps1 -PluginPath .tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll -ScanTimeoutSeconds 90
```

上述为已有构建和回归入口，历史结果见实现记录。本轮仅整理计划，尚未执行独立窗口、磁盘缓存和扫描按钮反馈的验收；需要按 P7.8 扩展这些脚本并取得新证据后，才能更新完成状态。提交检查遵循 [AGENTS.md](../AGENTS.md)。

### P7.8 剩余实施计划（2026-09-10）

按 R1 → R2 → R3 → R4 推进；R2、R3 作为同一轮扫描体验修改交付。以下均为待完成项。

#### R1：原生 GUI 使用独立窗口

- [ ] 将 editor 移到 Guitar Pro 主窗口所属的独立非模态顶层窗口，具备标题栏、关闭按钮和独立移动能力。窗口内部的 native child HWND 继续承载原生 `IPlugView`，窗口尺寸与 DPI 按插件能力处理。
- [ ] 继续使用正在处理音频的同一 component/controller 实例；首版沿用单个 editor 窗口，重复点击当前插件名称时恢复并聚焦已有窗口，切换插件时保存旧 state、卸载旧 view，再显示所选插件。
- [ ] 关闭 editor 只释放 view 并保存 state，效果继续启用；关闭或重建选择区不销毁 editor 和正在处理的实例。取消当前插件时关闭其 editor，再按已有实时读者排空流程释放实例；GP 退出时统一清理。
- [ ] editor 打开、移动和关闭均不改变选择区的布局、尺寸或命中区域；未启用项的名称点击继续给出先启用提示。

涉及：`qt_ui.cpp/.h` 的 editor 所有权及窗口管理，`gp_hook.cpp/.h` 的 view 关闭和尺寸回调。验收必须证明独立顶层窗口可见、可移动、可关闭、重复点击不重复创建、选择列表仍能勾选，以及关闭窗口后处理继续。不能只凭 `parent_name` 或状态文字断言通过。

#### R2：用本地 JSON 缓存扫描结果

- [ ] 在 `state::dataDirectory()` 下自动生成 `vst3-catalog-cache.json`，跨 GP 重启保留；该文件可删除并自动重建，使用现有 `QSaveFile` 原子写入。仅增加这一份派生缓存文件。
- [ ] 记录缓存 schema、扫描器兼容版本、x64 架构、扫描根目录、规范化 bundle 路径及文件指纹，并保存 class UID、名称、厂商、类别和扫描结果。文件指纹至少覆盖 bundle 清单、实际二进制大小/修改时间及元数据文件变化，避免只检查目录时间而漏掉插件更新。
- [ ] 启动时先读取缓存并展示清单，再在后台检查目录变化。缓存有效且插件未变化时复用结果，不逐个启动扫描进程或加载 factory；新增、更新只扫描变化项，删除项从可启用清单移除。再次打开选择区也异步检查变化，同一时刻只运行一项扫描任务。
- [ ] 缓存缺失、损坏或版本不兼容时自动后台重建；读取/写入失败时显示扫描结果并记录原因，保持 UI 可响应。开发用显式扫描路径与标准目录缓存隔离，避免测试结果污染正常清单。
- [ ] 缓存超时/失败结果及下次重试时间，未到重试时间的相同插件不因每次点击而重复等待 10 秒；文件变化后立即允许重扫。重试在后台进行，成功后更新缓存，失败不会永久拉黑插件，也不会覆盖其它插件的有效结果。
- [ ] 缓存命中只代表元数据可复用；实际启用仍校验文件、class UID、宿主哈希和实例初始化。扫描尚未结束或暂时超时时保留 sidecar 设置，不能把临时空列表当作插件已被卸载。

涉及：`vst3_host.cpp/.h`、`state_manager.cpp/.h`、`bootstrap.cpp` 和 `vst3_autoload.cpp`。验收需跨两个真实 GP 进程复用同一缓存；第二次启动记录 `cache_hit`、清单显示耗时和实际扫描次数，未变化的成功条目不再启动扫描进程。目标是在本机缓存命中后，点击入口到列表可操作不超过 500 ms，并记录冷启动基线与实测值。

#### R3：首次扫描时按钮立即给出提示

- [ ] 扫描状态同时驱动入口按钮和选择面板。按钮反馈先进入 Qt 绘制周期，扫描全程在后台执行；用户点击时立即看到选择面板或加载提示。
- [ ] 有真实进度时展示已完成/总数；尚未得到总数时显示不定进度，不使用虚构百分比。tooltip 可补充当前扫描插件名称及首次耗时说明。
- [ ] 扫描中继续允许打开/聚焦面板，连续点击不会重复扫描；有缓存时清单可先用，后台更新不重置当前勾选、正在处理的实例或已打开的 editor。音源区入口重建后也应显示同一进度。
- [ ] 完成、部分失败、全失败都有终态反馈；按钮不会一直停在扫描中。无可用结果时通过现有入口重试，不增加配置页面。

| 状态 | 按钮文案 | 面板提示及动作 |
| --- | --- | --- |
| 首次扫描、暂无可用缓存 | `VST3 · 正在扫描…`，有总数后为 `VST3 · 扫描 3/20` | `首次扫描可能需要一些时间，完成后将自动显示插件。`；列表随结果更新 |
| 已显示缓存、后台刷新 | `VST3 · 正在更新…` | 缓存清单可操作，提示正在检查插件变化 |
| 完成 | `VST3` | 显示清单并清除加载提示 |
| 部分失败或超时 | `VST3` | 显示可用插件，提示部分插件扫描失败，之后后台重试 |
| 全部失败且无可用缓存 | `VST3 · 扫描失败，点击重试` | 显示失败原因，再次点击提交一次重试 |
| 成功扫描但没有效果器 | `VST3` | `未发现可用的 VST3 效果器。` |

涉及：`qt_ui.cpp/.h`、扫描进度快照及 bootstrap 刷新流程。验收需在扫描未结束前经 MCP 读取按钮文字，连续点击后确认扫描任务仍只有一个；扫描成功和失败后分别核对按钮恢复及面板反馈。

#### R4：补齐回归与交付证据

- [ ] 扩展 Qt 夹具：独立窗口标志、原生 view 宿主尺寸、重复打开、关闭后仍启用、选择区重建，以及各扫描状态的按钮文案。更新当前仅断言 editor 是面板子控件的测试。
- [ ] 扩展扫描回归：无缓存首扫、同进程再次打开、重启命中、插件增删改、缓存损坏/旧 schema/写入失败、空目录、超时重试，记录缓存文件结果、耗时和扫描进程次数。
- [ ] 使用 GuitarProMCP 在含空格的隔离 GP 路径中按正常启动配置验证：首次按钮提示、重启缓存提速、ParametricOD/Gateway 原生独立窗口、切换/关闭/重开及音频处理。Qt 对象树无法证明窗口显示和布局时使用 MCP 截图补证；不使用 `computer-use`。
- [ ] 保留单/双实例串联、单项取消、全部直通、显式禁用、宿主哈希门控和 sidecar 恢复回归。对 GUI 参数联动，要记录参数编辑送达同一 processor 及可观测的输出变化；截图和“已打开”文字不替代音频证据。
- [ ] 更新实现记录和安装说明，构建发布包并验证文件白名单；清单缓存属于运行时生成文件，不随包分发。运行适用回归及 `git diff --check` 后提交实际修改，独立记录剩余宿主限制。

这三项交互修改及其验收全部通过后，才关闭本轮 P7 剩余工作。现有 master 后处理作用域与 P2/P4 的宿主限制继续单独记录。
