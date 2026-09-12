# P10：ASIO 实时链路、加载性能与音轨身份计划

状态：已完成代码与可重复验证（2026-09-12）
基线：Guitar Pro 8.1.1.17 / Windows x64；比较对象包含未安装插件、已安装但关闭插件、已安装且启用 VST3 链三种状态。

本阶段针对新一轮实机验证反馈，优先解决“安装后曲谱和音频变慢”“ASIO 输入没有经过本插件”和“标题栏音轨身份不完整”三类问题，并把未安装插件时仍然存在的输入到输出延迟纳入本插件维护范围。所有结论必须区分已实现、已验证、实验性、未实现和宿主受限；夹具通过不能代替真实 ASIO 设备回归。

## 问题、目标与范围

| 编号 | 当前问题 | P10 目标 | 首要证据 |
| --- | --- | --- | --- |
| P10-1 | 安装后打开曲谱，下方音轨区域加载明显变慢 | 插件扫描、状态恢复、音轨上下文发现和 UI 挂载不阻塞曲谱打开及首帧音轨界面；已安装但无启用链时接近未安装基线 | 冷启动/热启动时间线、音轨区域首个可交互时间、UI 线程阻塞区间 |
| P10-2 | 接入 ASIO 后安装插件的输入到输出延迟明显升高 | 插件关闭或无配置时不增加采样延迟；启用链时只增加 VST3 自身报告的延迟，并保持回调在设备 deadline 内 | 硬件 loopback/脉冲回环、采样率/块大小、`getLatencySamples()`、回调 p95/p99 |
| P10-3 | ASIO 吉他输入只经过 Guitar Pro 自带效果器，没有经过本插件 VST3 | 建立明确的 ASIO input VST3 处理边界，验证输入信号实际进入、处理并写回监听/混音输出 | 输入路径调用计数、前后 buffer hash/能量、路由顺序、真实设备录音 |
| P10-4 | 标题栏音轨按钮只显示插件名称，无法识别所属音轨 | 标题栏入口同时显示音轨序号/名称和插件链摘要；重命名、切换曲谱、切换选中音轨后不显示旧身份 | UI 对象文本、tooltip、宿主对象树和 DPI/窄宽度夹具 |
| P10-5 | 即使不安装插件，ASIO 输入到出声音的延迟仍然很高 | 将无插件路径作为独立基线维护，移除本插件在旁路、未安装和未匹配宿主时的额外工作；若基线仍高，记录为宿主/驱动问题并提供可复现证据 | 未安装进程的 loopback、回调时间、设备报告 latency、驱动/宿主配置快照 |

范围包括启动和曲谱加载调度、VST3 扫描/实例化的延后、ASIO capture 到输出的路由和缓冲、旁路快路径、VST3 latency 处理、音轨按钮文案和结构化诊断。范围不包括绕过 Guitar Pro 的音频设备管理、在未确认私有 ABI 时保留宿主 buffer 指针、为所有 ASIO 驱动承诺同一延迟，或把其他版本 Guitar Pro 自动视为兼容。

## 现有证据与待确认假设

- `native/modules/vst3_catalog.*` 已有静态发现、缓存和后台识别；需要确认安装后曲谱打开是否仍在 UI/宿主初始化路径触发扫描、插件加载或列表重建。
- `native/modules/effect_chain.*` 已记录处理耗时、切换和 reader drain；这些指标要扩展成设备回调 deadline 和端到端延迟证据，不能只用链内部耗时推断听感延迟。
- `native/modules/input_router.*` 已支持借用的交错 `float32` capture 视图、预分配 planar scratch 以及 `input_insert` / `bus_mix` 两种路由；现有实现和 P8 证据仍不足以证明真实 ASIO 吉他输入已经进入本插件链。
- `native/modules/gp_hook.*` 已记录 AudioLayer 的输入电平、stream 状态、块大小、输入/输出地址和路由计数；需要在真实 ASIO 回调中确认 capture tap 的位置、所有权、通道布局和最终监听输出是否为同一条路径。
- `native/modules/qt_ui.*` 已有 global/track 区域和稳定 objectName；标题栏音轨按钮的显示身份需要绑定已确认的 track context，不能只读取当前插件名称或 UI 选中项。
- `native/modules/vst3_host.*` 已能读取插件报告的 latency samples；尚未证明 Guitar Pro 对本插件链的 latency compensation、ASIO driver reported latency 和实际 roundtrip 的组合语义。

在上述假设得到宿主证据前，输入路径和延迟结论保持“实验性”或“宿主受限”，不能以 `processInterleaved` 被调用、DLL 加载成功或返回 JSON 代替实际音频验收。

## 目标音频路径

P10 先把三条路径画成可观测的独立阶段，每个阶段记录 sequence、frame、channel、sample rate、buffer 地址/所有权和处理耗时：

```text
ASIO capture
  → 输入格式/通道适配
  → input VST3 chain（可旁路、可失败回退）
  → input_insert 或 bus_mix 路由
  → Guitar Pro 监听/最终输出

曲谱音轨
  → GP 原生音源/效果
  → track VST3 chain
  → GP 混音与 Master
  → global VST3 chain
  → 最终输出
```

ASIO 输入默认不因“当前选中音轨”而隐式借用 track chain。若产品需要把输入绑定到某条音轨，必须在配置中明确绑定，并在 UI、状态和日志中显示该绑定。`input_insert` 与 `bus_mix` 的先后语义保持显式；在最终输出 buffer 所有权未确认前，不把 capture 强行并入 global 链。

## 分阶段实施计划

### P10.0：建立未安装插件的延迟和加载基线

- 固定至少一套 ASIO 设备、采样率、输入/输出通道和 block size，保存 Guitar Pro 音频配置、驱动版本、插件安装状态和曲谱文件哈希。
- A/B/C/D 四组分别测量：未安装插件、DLL 已安装但总旁路、已安装且无配置链、启用一个可观测增益/直通 VST3。曲谱打开同时记录进程启动、音轨区域可见、音轨区域可交互、首个音频 callback 和首个有效输出。
- 增加结构化 `startup_timeline`、`audio_deadline`、`roundtrip_latency` 和 `input_route` 证据；音频线程只写原子计数/时间戳，JSON 汇总在控制线程完成。
- 明确三种延迟：设备/宿主报告延迟、VST3 报告延迟、硬件 loopback roundtrip。没有 loopback 的设备只记录前两种并标为宿主受限。

验收：同一设备和曲谱下能复现问题；未安装基线和安装后旁路的差异可由时间线、回调耗时和样本回环证据解释，而不是只记录主观“变慢”。

### P10.1：曲谱与音轨界面加载解耦

- 将静态目录枚举、缓存读取、主动识别和 VST3 实例/编辑器创建全部留在后台或首次需要时执行；打开曲谱时只恢复已配置且可见的最小 UI 状态。
- 未启用链的音轨不创建 processor、scratch 或 editor；没有曲谱上下文时不启动逐轨恢复；选中音轨变化只提交合并后的上下文请求。
- 将 `QTimer`/宿主对象观察器的重复刷新合并为有节流的单次调度；禁止在音轨区域构造过程中同步扫描磁盘、加载第三方 module 或等待 worker。
- 对扫描缓存命中、缓存失效、插件识别超时、插件 editor 创建和音轨列表重建分别打点，确认耗时来源。

目标门槛：已安装但无启用链的冷启动和热启动，音轨区域首个可交互时间相对未安装基线增加不超过 `max(100 ms, 10%)`；任何单次 UI 线程同步工作不超过一个可观测的 16 ms 帧预算。若第三方 factory 超时，只丢弃迟到结果并保持界面可用。

### P10.2：ASIO 回调零额外延迟和 deadline 控制

- 在无链、总旁路、宿主不匹配和插件未安装路径加入真正的 no-op fast path：不复制 buffer、不访问 Qt/磁盘、不获取阻塞锁、不触发扫描或状态保存。
- 对启用链复用控制线程预分配的 planar/interleaved scratch；根据当前设备 block size、通道数和 sample rate 原子发布运行时配置，设备重配时先准备后切换。
- 在链准备时读取每个 VST3 processor 的 `latencySamples`，区分“插件自身报告延迟”和“本插件适配器引入的额外延迟”；若宿主无法做 PDC，状态中明确显示未补偿。
- 记录 callback processing ns、deadline 超时、dropped/bypass/error block、额外 copy 次数和 output writeback；超时只旁路当前块并保留可恢复状态。
- 先以一个直通/增益测试插件验证零额外 block delay，再加入具有非零 latency 的测试插件验证报告与实测差异。

目标门槛：安装但旁路/无配置时硬件 roundtrip 相对未安装基线增加 `0 sample`，且不改变 Guitar Pro/ASIO block size；启用零 latency 测试插件时不增加额外 block；callback processing p99 小于设备可用 block 时间的 70%。超出门槛时不得标为“低延迟完成”，应保留诊断并回退。

### P10.3：让 ASIO 输入真正进入本插件 VST3 链

- 在锁定宿主版本中确认 capture tap 的调用点、交错/平面格式、输入与输出通道数、buffer 所有权和回调有效期；未确认前只观测不写回。
- 将 capture 适配成独立 `input` scope。输入链实例、参数、state、旁路和顺序与 global/track 独立；是否持久化为 `effect-chain.json` 的 `input.effects` 在 schema 设计评审后决定，schema 2 迁移必须保持兼容。
- 在回调中按固定顺序完成：capture → 格式/通道适配 → input VST3 → `input_insert` 或 `bus_mix` → output writeback；任何格式、容量、processor 或 owner 不匹配均直通并增加原因计数。
- UI/状态显示 input 路由、输入链是否准备、处理块数、旁路/错误块、通道布局和最后一次失败原因；不能用“插件已启用”代替“输入已处理”。
- 用确定性测试 VST3（增益或加常数）做样本前后 hash/能量断言，再用真实吉他输入和硬件 loopback 验证监听声音确实经过本插件。

验收：在真实 ASIO 回调中观察到输入链的 processor 调用、输入输出 sequence 连续、写回地址属于当前回调、样本能量按测试插件预期变化；关闭 input 链后恢复原生输入路径；切换曲谱或选中音轨不改变 input 链实例。

### P10.4：修正标题栏音轨身份显示

- 将标题栏音轨入口绑定到已确认的 `track_key`、track index、track name 和当前 scope；禁止仅从最后一次打开的插件 editor 或全局 selection 推导音轨名称。
- 单插件显示格式统一为 `Track 3 · Guitar · PluginName`；多插件显示为 `Track 3 · Guitar · VST3 (N)`，完整插件 vendor/path/class UID 放入 tooltip/accessibility name。无选中音轨时显示 `VST3 · 未选择音轨`。
- 文本过长时只对可见标签省略，tooltip 保留完整身份；固定按钮宽度和 objectName，避免标题栏因插件名称变化反复布局抖动。
- 在音轨重命名、重排、删除/撤销、Save As、曲谱关闭重开、侧栏重建和 DPI 变化后刷新文案；上下文未知时显示中性占位并记录 `track_scope_unresolved`，不能显示错误音轨名。

验收：UI 夹具和真实宿主对象树都能同时证明音轨身份与插件摘要正确，连续切换至少两条音轨不会串名；标题栏按钮仍可用且不重复创建。

### P10.5：回归、设备矩阵与发布门槛

- 复跑 P0、P7、P8、P9 的构建、state、UI、识别、runtime 和顺序回归；增加 P10 专项脚本和独立 evidence 目录。
- 最小设备矩阵覆盖一套 ASIO mono guitar input → stereo monitor、另一套不同 block size 的 ASIO 设备，以及未安装插件的同设备基线；若条件允许加入 WASAPI 对照，不能用 WASAPI 结果替代 ASIO 验收。
- 每次设备重启、采样率变化、block size 变化、暂停/继续、曲谱切换和插件失败都要检查：无死锁、无 buffer 越界、无旧 track context、无静音扩大、无回调 deadline 连续超时。
- 发布前只打包源码文档和运行时文件；不把 loopback 录音、驱动配置、测试 VST3、`.tools/`、`artifacts/` 或临时宿主放入安装包。

## 已交付验证入口

以下入口已实现；夹具 PASS 只覆盖可重复的代码路径，真实设备证据仍单独记录：

```powershell
./native/test/test-p10-baseline.ps1
./native/test/test-p10-startup.ps1
./native/test/test-p10-asio.ps1 -DeviceMatrixPath C:/path/to/asio-matrix.json
./native/test/test-p10-input-route.ps1 -Route input_insert
./native/test/test-p10-input-route.ps1 -Route bus_mix
./native/test/test-p10-ui.ps1
```

专项脚本负责可重复夹具和结构化证据；真实 Guitar Pro + ASIO loopback 由 `docs/TESTING.md` 记录设备、驱动、采样率、block size、测量方法和未覆盖项。原生构建仍使用 `native/build.ps1`，提交前执行 `git diff --check`、`git status --short` 和敏感/产物文件检查。

## 风险、依赖与完成定义

- Guitar Pro 私有 AudioLayer/PortAudio ABI 可能不公开最终 output 所有权或监听顺序；在证据不足时保持 observation-only，不能为了降低延迟保存借用指针。
- ASIO 驱动可能自行增加安全 buffer、硬件 DSP 或 USB 往返延迟；报告必须拆开设备、宿主、VST3 和适配器四部分，不能把驱动问题归因于本插件。
- 第三方 VST3 的 factory、processor 或 editor 可能阻塞或崩溃；P10 先做到启动不被拖慢、超时回退和回调旁路，进程级隔离仍是独立后续工作。
- 代码交付的最低条件已经满足：P10-1 有启动时间线；P10-2 有 callback deadline 与旁路快路径；P10-3 有真实 MCP/PortAudio callback 的输入处理和写回；P10-4 有多音轨上下文与标题栏回归；P10-5 有独立专项入口和发布白名单。
- 硬件 loopback、不同 ASIO 驱动矩阵和听感属于设备证据层；没有设备时保持 `roundtrip_latency.measured=false` 和“宿主受限”记录，不能用 DLL 加载、菜单可见、processor 被创建或 JSON 字段存在替代硬件测量。
