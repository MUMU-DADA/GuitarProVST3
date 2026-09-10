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

开发回归统一通过[原软件免安装入口](P0_IMPLEMENTATION.md#原软件免安装测试2026-09-10)加载仓库中的 DLL，直接启动已安装的 Guitar Pro；不创建软件副本或向正式目录安装测试插件。宿主和开发 DLL 身份、原安装目录的文件完整性随回归记录。

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

**当前总状态：本轮 P7 的 R1–R4 已实现并通过验收（2026-09-11）。** 已实现独立原生 GUI、零第三方代码执行的静态发现、单项主动识别、跨进程缓存和扫描按钮反馈。真实宿主、音频参数和缓存证据见 [P7 实现记录](P7_IMPLEMENTATION.md)，验收清单保留在 [P7.8](#p78-剩余实施计划2026-09-10)。

### P7.1 交互和同级入口

- `gpvst3P7Panel` 与 `gpvst3SoundEffectChainButton` 在 MCP 真实宿主中确认是 `soundsContainer` 的直接子级，并沿用该区域的布局和重建生命周期。
- 已识别的 VST3 audio effect 使用复选框启停，插件名称负责打开原生 GUI；稳定 objectName 为 `gpvst3Enabled_<classId>`、`gpvst3Editor_<classId>` 和 `gpvst3NativeEditorHost`。静态信息不足的候选插件显示为“待识别”，允许用户主动选择该项后再识别，不得假定为兼容效果器或因缺少信息直接丢弃；具体边界见 R2。
- 面板重建会重新挂接入口，关闭面板后再次打开会重新创建对象；没有使用独立 dock 作为 P7 入口。
- 同级位置要求用于插件选择区。点击已启用插件名称后，其原生 GUI 应在 Guitar Pro 所属的独立非模态窗口中打开；选择列表应继续可见、可操作。

### P7.2 自动清单与静态扫描

**状态：已实现并验证。** `vst3_catalog.cpp` 仅遍历本地标准目录或显式路径，读取 PE x64 头、文件指纹、严格 JSON 元数据和派生缓存；不加载第三方模块、不运行扫描子进程、不请求网络元数据。缺少、损坏或不受支持的元数据保留为“待识别”，不会虚构 class UID，也不将静态识别称为运行兼容性验证。

`vst3-catalog-cache.json` 位于 `state::dataDirectory()`，包含 schema、扫描器版本、x64、规范化 roots、bundle 文件指纹和静态识别结果；同一文件按 roots 分 scope，隔离开发路径和标准目录。通过 `QSaveFile` 原子写入。启动先显示缓存，后台检查文件变化；打开入口及每 60 秒检查共用单个任务，读取错误按 60 秒重试期限缓存。缓存损坏、版本变化、写入失败和空目录均有专项证据。

动态 factory 枚举只在用户勾选具体待识别项时执行，失败保持未启用，刷新不重试动态加载。已保存启用链的恢复走独立运行时路径。完整生命周期探针必须显式设置 `GPVST3_RUN_LIFECYCLE_PROBE=1`，P1/P2 测试脚本已分开此入口。主动加载/恢复后的插件仍可能自行联网，未提供网络沙箱。

### P7.3 二态启用和实时链

**状态：已实现并通过 MCP 回归。** `checked = enabled`，旧 `bypass` 迁移为 `enabled = !bypass`。启用项在控制线程准备 component/controller/processor 和预分配 planar 缓冲，通过双槽链原子发布；两个实例按清单顺序串联，取消单项会复用其余实例，全部取消后直通。音频线程不扫描磁盘、不创建 Qt 对象、不动态分配、不持有 UI 锁。

默认启动的首次勾选会在宿主哈希/prologue 校验后启用实时接入，无需开发环境开关；显式禁用配置仍生效。含空格安装路径的自动扫描、默认启动勾选及失败回滚已补充验证，见 [正常启动后的勾选修复](P7_IMPLEMENTATION.md#正常启动后的勾选修复2026-09-10)。

### P7.4 原生 VST3 GUI

**状态：已实现并验证。** `gpvst3NativeEditorWindow` 是 GP 主窗口所属的独立非模态 `Qt::Window`；内部 `gpvst3NativeEditorHost` 提供 child HWND。复用处理链中同一实例的 `IPlugView`，保留 `IPlugFrame`、component/controller 连接和参数 mailbox。重复打开复用并聚焦，切换/关闭保存 state；选择区可通过关闭按钮销毁并重建，editor 和正在处理的实例继续存活。

插件请求的尺寸通过 Qt 调整 host 和窗口，提供 `IPlugViewContentScaleSupport` 的插件在打开和屏幕变更时接收缩放因子。已验证当前 96 DPI 下原生视图尺寸和 resize/move；多显示器不同 DPI 的实机组合未运行。ParametricOD/Gateway 的独立窗口与 caption 由 MCP 状态、Win32 属性及目标窗口截图交叉核验；GUI 参数到同一 processor 和实际 RSE 输出变化另由测试 VST3 验证。

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

新增静态目录/缓存、入口执行标记、慢扫描反馈、退出排空及 GUI 参数回归命令与证据见 [P7 实现记录](P7_IMPLEMENTATION.md)。第三方 HWND 的 MCP Qt 截图可能是黑图，不能单凭返回 image 判定 GUI 内容正确；本轮补充了仅针对测试 GP 进程窗口的 `PrintWindow` 截图。提交检查遵循 [AGENTS.md](../AGENTS.md)。

### P7.8 剩余实施计划（2026-09-10）

按 R1 → R2 → R3 → R4 推进；R2、R3 作为同一轮扫描体验修改交付。**2026-09-11 补充：扫描期间不得加载或执行第三方插件代码，该要求覆盖首次扫描、后台刷新、手动重扫和缓存重建。** R1–R4 已完成；以下保留本轮验收条目，具体构建、实测值与发布包证据以实现文档为准。

#### R1：原生 GUI 使用独立窗口

- [x] 将 editor 移到 Guitar Pro 主窗口所属的独立非模态顶层窗口，具备标题栏、关闭按钮和独立移动能力。窗口内部的 native child HWND 继续承载原生 `IPlugView`，窗口尺寸与 DPI 按插件能力处理。
- [x] 继续使用正在处理音频的同一 component/controller 实例；首版沿用单个 editor 窗口，重复点击当前插件名称时恢复并聚焦已有窗口，切换插件时保存旧 state、卸载旧 view，再显示所选插件。
- [x] 关闭 editor 只释放 view 并保存 state，效果继续启用；关闭或重建选择区不销毁 editor 和正在处理的实例。取消当前插件时关闭其 editor，再按已有实时读者排空流程释放实例；GP 退出时统一清理。
- [x] editor 打开、移动和关闭均不改变选择区的布局、尺寸或命中区域；未启用项的名称点击继续给出先启用提示。

涉及：`qt_ui.cpp/.h` 的 editor 所有权及窗口管理，`gp_hook.cpp/.h` 的 view 关闭和尺寸回调。验收必须证明独立顶层窗口可见、可移动、可关闭、重复点击不重复创建、选择列表仍能勾选，以及关闭窗口后处理继续。不能只凭 `parent_name` 或状态文字断言通过。

#### R2：静态扫描、按需加载和本地 JSON 缓存

- [x] 将启动、打开选择区、手动重扫和缓存重建统一为静态文件发现：仅遍历本地 `.vst3` 文件/bundle、检查文件指纹及架构、读取 `Contents/Resources/moduleinfo.json` 等静态元数据或有效缓存。扫描不得调用第三方 `LoadLibrary`/`LoadLibraryEx`、`InitDll`、`GetPluginFactory`、实例创建或处理接口，也不得启动子进程代为执行这些操作或联网获取元数据。
- [x] 对静态元数据做格式、class UID 和所属 bundle 校验，区分信息已识别与运行兼容性已验证。元数据缺失、损坏、不受支持或缓存失效时，保留路径/文件名并显示“待识别”；不虚构 class UID，不自动动态探测，也不将候选插件直接判定为已兼容或已卸载。
- [x] 仅在用户主动选择某个待识别项时，针对该 bundle 动态枚举并校验效果器类别、class UID，再走既有启用流程；禁止顺带加载其余候选项。识别失败保持未启用并给出原因，只有用户再次选择才重试。名称点击打开已启用插件 GUI、已保存启用链的恢复继续属于独立运行时流程，不能借此初始化未启用插件来补全扫描清单。
- [x] 明确执行与联网边界：静态扫描不加载第三方代码、不发起外联；主动选择或恢复已启用项后，插件代码仍可能自行联网，不能把此阶段宣称为离线或已沙箱化。进程隔离不能作为网络隔离证据，也不能用全局防火墙改动替代静态扫描要求。
- [x] 在 `state::dataDirectory()` 下自动生成 `vst3-catalog-cache.json`，跨 GP 重启保留；该文件可删除并自动重建，使用现有 `QSaveFile` 原子写入。仅增加这一份派生缓存文件。
- [x] 记录缓存 schema、扫描器兼容版本、x64 架构、扫描根目录、规范化 bundle 路径及文件指纹，并保存可取得的 class UID、名称、厂商、类别、信息来源和识别状态。文件指纹至少覆盖 bundle 清单、实际二进制大小/修改时间及元数据文件变化，避免只检查目录时间而漏掉插件更新。
- [x] 启动时先读取缓存并展示清单，再在后台静态检查目录变化。缓存有效且插件未变化时复用结果；新增、更新项只读取变化文件和元数据，信息不足就转为“待识别”，删除项从可启用清单移除。再次打开选择区也异步检查变化，同一时刻只运行一项静态扫描任务。
- [x] 缓存缺失、损坏或版本不兼容时自动在后台按静态流程重建，禁止回退到旧动态扫描；读取/写入失败时显示可取得的结果并记录原因，保持 UI 可响应。开发用显式路径与标准目录缓存隔离；完整生命周期探针必须作为明确的开发测试入口执行，不能由启动或重扫路径隐式触发。
- [x] 缓存文件读取/解析失败结果及下次重试时间，后台重试只进行静态读取，文件变化后允许重新检查。缺少静态元数据和历史动态扫描超时均不能触发自动加载重试；不永久拉黑插件，也不覆盖其它插件的有效结果。
- [x] 缓存命中只代表元数据可复用；实际启用仍校验文件、class UID、宿主哈希和实例初始化。扫描尚未结束、信息不足或暂时读取失败时保留 sidecar 设置，不能把临时空列表当作插件已被卸载。

涉及：`vst3_host.cpp/.h`、`state_manager.cpp/.h`、`bootstrap.cpp`、`vst3_autoload.cpp` 及 `qt_ui.cpp/.h` 的待识别项交互。验收需跨两个真实 GP 进程复用同一缓存；记录 `cache_hit`、清单显示耗时、静态文件检查次数及第三方模块加载次数。首次与后续扫描的第三方代码执行次数都必须为零，不能仅证明缓存命中时不加载。目标是在本机缓存命中后，点击入口到列表可操作不超过 500 ms，并记录冷启动基线与实测值。

#### R3：首次扫描时按钮立即给出提示

- [x] 扫描状态同时驱动入口按钮和选择面板。按钮反馈先进入 Qt 绘制周期，扫描全程在后台执行；用户点击时立即看到选择面板或加载提示。
- [x] 有真实进度时展示已检查插件数/总数；尚未得到总数时显示不定进度，不使用虚构百分比。tooltip 可补充当前检查的插件名称。静态扫描进度与用户主动选择后的单项识别/加载反馈分开，不能为了补全进度执行插件代码。
- [x] 扫描中继续允许打开/聚焦面板，连续点击不会重复扫描；有缓存时清单可先用，后台更新不重置当前勾选、正在处理的实例或已打开的 editor。音源区入口重建后也应显示同一进度。
- [x] 完成、部分失败、全失败都有终态反馈；按钮不会一直停在扫描中。无可用结果时通过现有入口重试，不增加配置页面。

| 状态 | 按钮文案 | 面板提示及动作 |
| --- | --- | --- |
| 首次扫描、暂无可用缓存 | `VST3 · 正在扫描…`，有总数后为 `VST3 · 扫描 3/20` | `首次扫描可能需要一些时间，完成后将自动显示插件。`；列表随结果更新 |
| 已显示缓存、后台刷新 | `VST3 · 正在更新…` | 缓存清单可操作，提示正在检查插件变化 |
| 完成 | `VST3` | 显示清单并清除加载提示 |
| 存在缺少静态信息的候选插件 | `VST3` | 对应项显示“待识别”；用户主动选择该项后再识别，其余候选项不加载 |
| 部分文件读取或解析失败 | `VST3` | 保留已识别及待识别项，提示部分文件读取失败，之后只重试静态读取 |
| 全部失败且无可用缓存 | `VST3 · 扫描失败，点击重试` | 显示失败原因，再次点击提交一次重试 |
| 成功扫描且没有效果器或待识别候选 | `VST3` | `未发现可用的 VST3 效果器。` |

涉及：`qt_ui.cpp/.h`、扫描进度快照及 bootstrap 刷新流程。验收需在扫描未结束前经 MCP 读取按钮文字，连续点击后确认扫描任务仍只有一个；扫描成功和失败后分别核对按钮恢复及面板反馈。

#### R4：补齐回归与交付证据

- [x] 扩展 Qt 夹具：独立窗口标志、原生 view 宿主尺寸、重复打开、关闭后仍启用、选择区重建，以及各扫描状态的按钮文案。更新当前仅断言 editor 是面板子控件的测试。
- [x] 扩展静态扫描回归：无缓存首扫、同进程再次打开、重启命中、手动重扫、插件增删改、缓存损坏/旧 schema/写入失败、空目录，以及 `moduleinfo.json` 缺失/损坏/不受支持和静态读取重试。核对缓存文件、待识别状态、耗时，以及上述所有路径均无自动动态探测。
- [x] 在独立测试插件目录和空启用链条件下，通过模块加载事件或带可观察入口标记的测试 VST3，证明扫描期间宿主及其子进程没有加载第三方插件、入口执行次数为零。结合按进程归属的网络记录确认扫描无外联；GP 原有网络活动和 MCP 本机通信单独归因，不能用“没弹防火墙提示”或仅返回 JSON 作为证据。
- [x] 验证主动选择一个待识别项时仅加载该 bundle，完成类别校验、启用或明确失败回滚，其余候选项入口仍为零；失败后刷新/重开面板不自动重试动态加载。已保存启用链的恢复单独验证，不混入静态扫描的零执行指标。
- [x] 使用 GuitarProMCP 通过免安装入口启动原软件，在实际含空格的安装路径中按默认 hook/扫描配置验证：首次按钮提示、重启缓存提速、ParametricOD/Gateway 原生独立窗口、切换/关闭/重开及音频处理。Qt 对象树无法证明窗口显示和布局时使用 MCP 截图补证；不使用 `computer-use`。
- [x] 保留单/双实例串联、单项取消、全部直通、显式禁用、宿主哈希门控和 sidecar 恢复回归。对 GUI 参数联动，要记录参数编辑送达同一 processor 及可观测的输出变化；截图和“已打开”文字不替代音频证据。
- [x] 更新实现记录和安装说明，构建发布包并验证文件白名单；清单缓存属于运行时生成文件，不随包分发。运行适用回归及 `git diff --check` 后提交实际修改，独立记录剩余宿主限制。

独立窗口、静态扫描与按需加载、持久化缓存、按钮反馈及对应验收现已通过，本轮 P7 剩余工作已完成。现有 master 后处理作用域与 P2/P4 的宿主限制继续单独记录。
