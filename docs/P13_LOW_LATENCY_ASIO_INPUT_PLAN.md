# P13：ASIO 输入低延迟监听与 RSE 共存计划

状态：0.10.1 已修订输入开关、插件启停、UI、空链监听、多率/大 buffer 与实时处理开销（2026-09-20）。离线专项及真实宿主开关、三链、冷启动与编辑器重入回归已通过；全硬件矩阵仍未完成；0.10.2 收尾时用户确认当前演奏场景在开启声卡驱动安全模式后爆音消失。0.10.0 的 ASIO 192000 Hz / 64 帧固定配置验收仅作历史证据。当前结果见 [P13 实现记录](P13_IMPLEMENTATION.md) 和 [宿主边界调查](P13_HOST_BOUNDARY.md)。

本阶段目标是降低实时输入监听延迟：在验证 ASIO 输入到宿主 callback 之间没有额外大块聚合后，使用实际输入 block 进入独立 VST3 输入链，同时保留 Guitar Pro RSE 音源、原生效果器链以及现有 global/track VST3 音频逻辑。两路必须可以同时发声，在设备输出边界合成；保留 RSE 原有渲染线程和调度，不要求 RSE DSP 与输入 DSP 都嵌套在同一 callback 内执行。

当前代码仅在 Guitar Pro LINE-IN 已开启且低延迟模式已请求时运行独立 input overlay；原生监听的 output 在进入共享混音前分流，原生 DSP 继续运行。已实现 44100、48000、88200、96000、176400、192000 Hz 对应拓扑，32、64、128、256、512、1024、2048、4096、8192 帧已有离线分块覆盖。实际支持仍受宿主 hash、stream identity、generation、rate 与 SRC 拓扑共同门控，范围外或合同不符报告 `host_limited`。这不是硬件全矩阵通过声明。录音要求按 GP8 官方手册判为宿主不提供/不适用，不能用实验 PCM recorder 代替。

## 1. 目标和不变约束

### 1.1 目标

1. Guitar Pro LINE-IN 已开启且明确请求低延迟输入模式时，真实 ASIO 输入直接进入独立的 input VST3 chain；空链按监听增益干声直通。保存模式偏好不能自行开启宿主输入。
2. input VST3 的 `ProcessData.numSamples` 使用经验证的当前处理段实际 `frames`；大 callback 在同次调用内按不超过 2048 帧分段，分别记录 ASIO 驱动 buffer 与处理帧数，不把两者默认视为相同。
3. RSE 播放保留现有原生音轨效果、track VST3、原生 Master 效果和 global VST3 的处理顺序、块大小与调度。
4. RSE 生成的 output 与输入 VST3 的结果在 callback 内叠加，输入监听不能覆盖 RSE 播放声。
5. 首次准备失败时保留原路由；激活后的输入处理故障只静音输入贡献并保留 GP 输出；关闭模式时解除原生监听分流，遵从用户当前 LINE-IN 开关，不恢复旧选择。

### 1.2 不变约束

- 不替换 ASIO driver，不创建第二个声卡 stream，不修改 Guitar Pro 的全局音频设备生命周期。
- 不改变 `Master::process`、`EffectsChain::processDSP` 的调用顺序和 buffer 语义。
- 不改变 RSE 音源、GP 原生效果器、现有 global/track VST3 实例的参数、状态、顺序和处理方式。
- input VST3 实例、processor、scratch、参数 mailbox 与 RSE/global/track 实例完全隔离。
- 新模式的插件选择、启停、参数、state、持久化、故障和 editor 归属独立于 global/track；已有输入实例独立不代表这些控制状态已经独立。
- 音频线程不分配、不访问 Qt、不读取 sidecar、不等待控制线程锁；所有实例、scratch 和格式切换在控制线程准备。
- 不在未验证输入边界时用静音替换宿主输入。静音替换只能作为通过宿主证据后的显式低延迟模式实现，不能成为默认行为。

## 2. 改造前链路与当前接入边界

以下表格和第一幅调用图记录 P13 开始前的 P4 输入路径，作为需要隔离的历史基线；其中参数镜像、global preload 联动与覆盖 output 的行为只描述 legacy 路由，不是当前独立 input 的控制方式。

改造前已有两条不同入口：

```text
RSE / 播放：
RSE 音源 → 原生音轨效果 EffectsChain::processDSP → 现有 track VST3
         → GP 混音 / 原生 Master 效果 → 现有 global VST3 → GP 设备输出

改造前 hook 调用顺序：
PortAudio input → streamCallbackHook → GP 原始 stream callback
                                   → 现有 input_router → output
```

上图是逻辑信号顺序，不代表所有 RSE 处理都发生在设备 callback 线程。P13-0 后续已定位 GP 内部 listener、共享 output SRC/ring 与混音位置，见 [宿主边界调查](P13_HOST_BOUNDARY.md) 和 [排空分析](P13_SRC_DRAIN_ANALYSIS.md)。

改造前源码证据和限制（保留原问题，不作为当前能力声明）：

| 入口 | 已确认行为 | 不能据此推导的结论 |
|---|---|---|
| [gp_hook.cpp](../native/modules/gp_hook.cpp) 的 `streamCallbackHook`、`processExternalInputInterleaved` | 先调用 GP 原始 callback，再验证配置并将交错 float32 输入送入 router | 该输入一定等于硬件 ASIO 小块、原生输入效果已经被旁通 |
| [portaudio_capture_abi.h](../native/modules/portaudio_capture_abi.h) | 读取设备 ID、采样率与通道；当前校验支持 1/2 通道、交错 float32；`kMaxFrames=2048` 是容量门控 | 实际 callback 固定为 2048 或 1024；设备 ID 本身证明 ASIO backend |
| [audio_adapter.cpp](../native/modules/audio_adapter.cpp) 的 `process` | `ProcessData.numSamples = block.frameCount` | `setupProcessing.maxSamplesPerBlock` 是每次必须填满的帧数 |
| [gp_hook.cpp](../native/modules/gp_hook.cpp) 的 `configureInputSelection` | input 有独立 `inputChain`、双槽、实例池，准备容量使用 `16384` | 输入选择和控制状态已经独立；16384 是实际处理长度 |
| 同文件的 selection worker、`refreshInputParameterMirrors`、`mirrorInputParameter`、`setTotalBypass` | global 选择会配置 input，参数会镜像到 input，总旁路会同时改变 global 与 input router | 可直接沿用现有控制逻辑而满足两路互不影响 |
| 同文件的 `RuntimeEffect::captureState` 和 global preload 分支 | 保存 state 会更新 input 镜像实例的 identity；global 预加载会填充 input pool | 只解除参数转发就足以隔离 state 与缓存 |
| [input_router.cpp](../native/modules/input_router.cpp) | `input_insert` 写回时覆盖 output；`bus_mix` 先混合后处理；错误可回退干声，`interleave` 会限幅到 `[-1,1]` | 旧路由或整块重写路径已经满足 overlay 的不变性与故障合同 |

原生音轨 DSP 后接 track VST3、原生 Master 后接 global VST3 的顺序另见 [P8 范围与验收](P8_TRACK_GLOBAL_VST3_PLAN.md)。P13 不改变这两个现有处理点。

普通构建新增的低延迟 input overlay 路径：

```text
真实 ASIO input ─────────→ 独立 input VST3 chain ──┐
                                                   ├→ output
RSE/现有 GP output ────────────────────────────────┘
```

输入 VST3 只处理真实 capture，不处理 RSE generated output。低延迟模式必须使用 overlay 语义，不能复用当前 `input_insert` 的“覆盖 output”语义，也不能把 RSE output 送入当前 `bus_mix` 后再经过 input VST3。

独立 overlay 路由保留 `input_insert` / `bus_mix` 的既有语义和回归。新模式退出 global 选择同步、参数/state 镜像和预加载关联，缓存按 input scope 管理；global/track 的现有操作仍只按其原有 RSE 语义生效，不得重建、改参或启停新 input 链。legacy 模式的兼容关联单独保留。当前普通构建已实现这些隔离，真实三 scope 矩阵与离线故障专项分别见实现记录。

## 3. 输入旁通合同

### 3.1 旁通目标

“旁通自带效果器”只针对 Guitar Pro 的实时输入监听效果路径。它不旁通 RSE 音源生成，也不旁通 RSE 播放所需的原生音轨/global 效果器。

启用低延迟模式时，宿主 callback 的输入旁通需要通过已验证的输入边界实现：

1. 确认原生输入监听分支、效果器、队列与混入输出的位置，优先寻找混音前可独立旁通的输入监听边界。
2. 在该边界排除原生输入的干声、湿声、历史排队数据和 delay/reverb 尾音；不得清空或重置与 RSE 共用的 DSP 状态。
3. “向原始 callback 提供预分配静音输入”仅作为候选实验。它只能阻止新输入，不能证明原生 DSP 停止运行或旧尾音消失；只有证明原生输入贡献被完整隔离且 RSE、录音、输入电平和宿主状态不受影响，才能采用。
4. 用真实 ASIO input 走独立 input VST3 chain，再将处理结果叠加到保留的 GP output；不得同时保留原生监听副本。
5. 若候选方式影响录音、电平或共享状态，继续定位更精确的监听分支；找不到满足合同的边界则报告 `host_limited`，不启用该模式。

当前实现选择 listener 输出末端分流，不向原始 callback 提供静音 capture；原 listener 仍接收真实输入并执行一次，其输出写入独占 sink。之后在不重置共享状态的前提下排空 output SRC/ring，下一 callback 才提交 overlay。此处“输入效果旁通”表示排除监听贡献，不表示停止原生 DSP 计算。

录音门禁适用性：GP8 [官方用户手册](https://static.guitar-pro.com/gp8/manual/Guitar-Pro-8-user-guide.pdf) PDF 第 315 页（印刷第 309 页）明确说明 “There is not any record feature in Guitar Pro 8”。因此原生音频录制在本版本记为宿主不提供/不适用，MIDI 音符输入和导入 Audio Track 不能冒充音频录制验收；输入电平与宿主状态要求继续保留。

低延迟模式不能只在原始 callback 返回后删除输入效果结果：此时输入效果已经可能与 RSE output 混合，无法可靠拆分。

还必须确认 live input 是否在共享 Master 压缩器、限制器或其他有状态/非线性 DSP 之前汇入。若存在此类交互，移除 input 可能改变共享 DSP 的状态及 RSE 响应；即使函数调用和参数不变，也不能宣称保持了原混合场景的 RSE 效果。P13-0 必须验证这一点，无法独立隔离时按当前不变性要求判为 `host_limited`，不得以 RSE-only 基线通过代替共存证明。

### 3.2 callback 内调用顺序

在已确认的接入边界采用下面的顺序。大 callback 先拆为不超过 2048 帧的连续处理段，每段遵循同一流程，原函数提前结束时保留其返回状态并清零未处理尾部：

```text
streamCallbackChunk
  ├─ 核对 LINE-IN 当前状态并获取 mode / slot / stream generation 配置快照
  ├─ 验证输入边界、格式、通道、frames、容量和指针
  ├─ 必要时将真实 input 复制到预分配 scratch，防止原 callback 改写别名数据
  ├─ 按已验证的合同抑制原生输入监听贡献
  ├─ 调用 GP 原始 callback
  │    └─ 保留原有调度，得到不含原生输入监听贡献的 GP output
  ├─ 同一配置快照下：真实 input → input VST3 chain（空链直通）→ 独立 scratch
  ├─ 校验成功后将 inputGain * processedInput 叠加到 GP output
  ├─ 补齐原生 [-1,1] 设备输出范围并记录削波
  └─ 返回原始 callback 状态
```

改造前的配置验证在原始 callback 之后；当前独立监听必须在 listener 分流之前验证配置、指针及身份，不允许仅凭启用开关写入或替换指针。借用的设备指针不得保留到 callback 之外；slot 退出使用后由控制线程回收。静音 input 仍仅是历史候选实验，正式实现不采用。

原生输入抑制和 overlay 发布必须共用同一份配置快照并在块边界原子提交，避免一次 callback 中出现两份监听或模式前后不一致。

存在 output SRC/ring 的切换先进入 `draining`：此时 native 贡献已抑制而 overlay 尚未输出。只有经过计数证据确认旧数据排空，才在后续块入口进入 `active`；这段有界数据排空的监听空隙不能显示为已生效。44100 Hz 无 output SRC/ring 时核对直通拓扑后不等待不存在的历史队列。停止或暂停时无数据推进，不以墙钟超时强制激活。

### 3.3 混音与故障合同

```text
mixedOutput = unchanged GP/RSE output + inputGain * processedInput
deviceOutput = clamp(mixedOutput, -1, 1)
```

- 输入采用明确的增益、固定通道映射和有限值检查；输入处理和试混音全部成功之前不改写 GP output，错误时丢弃整块输入结果，包括加法溢出。
- 不动态归一化、压缩或延迟 RSE 来适配输入。提供手动输入增益与削波检测，留出相加的余量；overlay 叠加后补齐 AMAudio 原有 `[-1,1]` 输出范围并计数，避免绕过原生最后一道输出边界。削波时合成声音会改变，因此 RSE 不变性以叠加前信号和未削波条件验收。
- 首次准备失败或宿主合同未通过：不提交低延迟模式，原始路由保持不变。空 input 链是合法干声监听配置，采用同一宿主开关、格式门控、排空和增益流程。
- 激活后发生插件返回错误、非有限输出、容量不足或重配置：保留 GP output，静音 input overlay，继续抑制原生输入监听；不得自动放出干声或原生湿声。流重建时按新 generation 重新验证，旧指针失效后不得继续替换输入。
- 用户明确关闭模式时解除分流并遵从 LINE-IN 当前状态；宿主关闭输入时立即停止输入贡献，不得因保存的模式或旧开关状态重新开启输入，也不能一律切到 legacy。
- 设备 callback 内的原始处理、输入处理和混音共用 `frames / sampleRate` 的时间预算。实例隔离不代表 CPU 或崩溃隔离；进程内第三方插件超时、死锁或崩溃仍可能影响整个宿主。

## 4. 运行时状态和配置

新增独立于 global/track 的输入状态：

```text
input_monitor_mode = off | legacy | low_latency_overlay
input_runtime_state = off | legacy | waiting_for_input | preparing | draining | active | muted | host_limited
input_native_effect_bypass = false | true
input_asio_driver_frames
input_callback_frames
input_process_frames
input_prepared_capacity
input_bypass_reason
```

语义：

- `off`：本插件的输入扩展关闭，不代表强制关闭 GP 原生监听；退出低延迟模式后继续遵从 LINE-IN 当前状态。
- `legacy`：保持现有 `input_insert`/`bus_mix` 兼容行为，便于回滚和旧测试。
- `low_latency_overlay`：LINE-IN 开启且合同通过时旁通原生输入贡献，真实 input 独立进入 VST3（空链干声直通），再与 GP output 叠加。LINE-IN 关闭或状态未知时为 `waiting_for_input`，不分流或处理输入。

低延迟模式不允许把 `bus_mix` 当作别名。若用户选择了不支持 overlay 的路由，UI/API 必须返回明确状态并保持旧模式。

低延迟开关同时控制原生输入监听旁通和 overlay 激活；`input_native_effect_bypass` 是实际生效状态，不作为允许双重监听的第二个独立开关。用户请求与实际状态分开保存。驱动 block 无可靠证据时显示未知，不能用 callback `frames` 填充冒充驱动值。

输入链使用独立 scope 保存插件列表、顺序、启停、参数/state、增益和 editor 归属；复用现有持久化机制并兼容旧数据，不能改写 global/track 的字段。启用、取消、停用及 editor 操作采用与其他 scope 一致的待提交/实际运行状态确认；请求中不能假报已生效。editor 的控制器与参数必须属于 input 实例。切谱、切轨与 global 修改不得触发输入重建或参数同步。

`setupProcessing.maxSamplesPerBlock` 是准备容量；每次 `process` 的 `numSamples` 才是实际帧数。在容量以内的可变 `frames` 直接处理，不填满固定长度、不另增整块 FIFO、不因每次块长变化重新 setup。4096/8192 帧在同一次设备 callback 内按 2048 帧连续处理，推进输入/输出指针及 ADC/DAC 采样时间，保持该次调用的 `currentTime` 基准；原函数提前结束时清零尚未处理的尾部。采样率、通道、stream generation 变化或所需容量增长由控制线程重新准备 slot；callback 不调用 `setupProcessing`、分配内存或销毁实例。

P13-0 已发现本机 ASIO 实际 rate 可为 192000，而 `PaStreamInfo.sampleRate` 仍为 GP 请求值 44100，GP 在内部转换两者。低延迟 input slot 必须按真实 capture rate 准备并绑定当前 stream generation；不能用旧 `portaudio::Configuration.sampleRate`、RSE rate、callback 间隔推算值或 UI 设置代替。驱动 rate 在控制线程独立查询与验证，callback 不调用 driver 控制接口；无法保证当前 generation 的实际 rate 时拒绝激活或静音输入贡献。

激活中的重配置期间输入静音、RSE 照常运行；无法在新流上安全隔离原生输入时，必须停用该流上的 overlay 并报告 `host_limited`，不能宣称仍满足低延迟模式合同。

## 5. 实施阶段

### P13-0：ASIO 数据路径和宿主输入边界确认

- 由开发侧检查匹配版本的 DLL、反汇编与真实运行数据，保存模块版本/hash、函数/RVA、调用链和观测记录；分别定位 ASIO 驱动回调、PortAudio 适配层、GP 输入监听效果与最终设备输出。
- 记录实际 backend/device、驱动 buffer、宿主 callback `frames`、采样率、队列深度与块适配行为。设置值、ABI 容量常量或 DLL 中单个 `1024` 常量均不足以证明运行块长或延迟来源。
- 若驱动为 128 帧而当前 hook 为 1024 帧，确认聚合位置；当前 hook 单独使用不能达标。必须找到同时满足小块 capture、同周期输出写回和 RSE 共存的更早接入边界，否则标记 `host_limited`。将已积累的 1024 拆成 128 不会恢复已损失的延迟。
- 使用有界诊断记录 frames、采样率、线程、序列、时间戳和必要的短样本；只在实验窗口采样，不持续在实时线程做全量 hash、统计分位数或写盘。无有效数据保证的原始 output 不用于音频证据。
- 增加“原始 callback 使用静音 input”的实验开关，默认关闭；只在匹配的 Guitar Pro 8.1.1.17 和副本曲谱中运行。
- 使用可区分的 RSE 和输入信号，检查混音前贡献、共享非线性 DSP 的交互、原生输入队列、长尾效果、录音/电平状态和 RSE/global/track 顺序；不能只凭计数相同认定声音相同。
- 输出可实施的边界合同：格式、指针生命周期/别名、线程、输入抑制位置、旧数据/尾音处理、流停止与重建规则、版本门控。

退出条件：同时证明小块输入与及时输出可达、原生输入贡献能独立隔离、RSE 路径保持原样。任一项不能证明则记录具体阻塞，保持计划/实验状态，不把后续单测通过当作宿主可行性证明。

### P13-1：独立 input overlay

- 在 `input_router` 增加 overlay 处理，不覆盖 GP output。
- 复用独立 `inputChain` 和 scratch；禁止复用 global/track processor。
- 解除新模式与 `configureInputSelection(global)`、参数镜像、`captureState` identity 同步、global preload 和 `setTotalBypass` 的联动，建立独立 input 选择、控制器、缓存和配置所有权；legacy 关联保持兼容。
- 处理 64/128/256/512/1024 及容量内非固定帧数的 callback，验证 `numSamples` 等于实际输入，块长变化不触发 setup 或实例重建。
- 低延迟模式下只处理真实 capture，绝不把 generated output 送入 input VST3。
- 验证输入/输出别名、空指针、容量边界、单声道/立体声映射和 scratch 写回；故障时静音输入贡献，不调用旧路由的干声 fallback。

### P13-2：输入 native effect 旁通

- 仅在 P13-0 合同通过后启用已验证的输入监听分支旁通；静音 input 候选必须满足同等合同。
- 在块边界提交输入抑制、slot、路由和 generation 的一致快照；请求接收与冷启动准备完成分开记录，不能承诺新插件在下一个 callback 就准备好。
- 验证长尾效果、排队旧数据和快速切换的过渡；不得重置共享 RSE 状态。切换期间不能销毁仍被音频线程使用的 slot。
- 保留输入电平和宿主状态的行为证据；退出分流时遵从 LINE-IN 当前状态，不强制打开输入。原生录音按宿主不提供处理。

### P13-3：UI、诊断和持久化

- 输入窗口集中显示“低延迟输入监听”、增益和当前状态；插件列表沿用其他 scope 的启停与 GUI 操作流程，设备详情默认折叠。等待 LINE-IN、空链干声、准备、排空、运行与故障分别显示。
- 显示已验证的驱动 buffer、`input_callback_frames`、采样率、通道数、输入链延迟样本、overlay/muted 原因和错误计数。串行插件延迟按实际链路累计，并响应延迟变化通知。
- 低延迟模式的用户意图与运行时状态分开保存；宿主不支持时保持配置、拒绝激活，并显示原因。
- RSE/global/track 的现有开关、参数、编辑器和 sidecar 不复用输入模式字段。

### P13-4：回归和发布门禁

- 通过静态检查、overlay/旧路由专项、可变帧数与容量测试、别名测试、输入故障和状态独立性测试。
- 在真实 Guitar Pro 8.1.1.17 中同时播放 RSE 和实时 ASIO 输入，分离观测叠加前 RSE 与输入贡献；确认两种信号同时存在，RSE 仍经过原有全部原生与 VST3 效果。
- 确定性夹具比较逐样本相等；含随机调制等第三方插件采用一致状态、采样对齐和预先说明的误差标准，不要求任意两次播放 hash 一致，也不比较含新增输入的最终混音与 RSE-only 基线 hash。
- 运行输入开关、尾音/快速切换、RSE 切轨/切谱、global/track 启停改参、两路使用同一插件的独立 editor/state、输入插件替换和故障组合矩阵。
- 记录输入处理及整次 callback 的时间分布、deadline miss/xrun；在相同设备、采样率、驱动 buffer 下测量改造前后输入到输出延迟，记录驱动报告值与物理回环的测量口径。
- 保留 Standard/ASIO、不同 buffer、单声道/立体声、采样率/设备/stream 重建证据；Standard 保持原行为。未运行的真实设备矩阵标为未验证，不以构建或离线单测代替。

## 6. 验收门槛

| 项目 | 必须满足 |
|---|---|
| RSE 共存 | 两路信号同时存在，原有 `Master`/track/global VST3 顺序、参数、块处理和调度保持原样 |
| RSE 结果 | 叠加前 RSE 贡献满足确定性逐样本比较或已定义的第三方插件误差标准，确认共享非线性 DSP 不因输入改道改变 RSE 响应；不可用最终混音 hash 代替 |
| 输入 block | 已证明 ASIO 小块可达和输出及时写回；`numSamples = frames <= preparedCapacity`，不新增整块等待，容量内变长不重新 setup |
| 输入旁通 | 排除原生输入干湿声、排队数据和尾音，不修改 RSE playback path、录音或电平语义 |
| 同时运行 | overlay 在最终输出边界加法合成；新模式不使用旧 `input_insert` / `bus_mix`，旧模式语义不变 |
| 隔离 | input 实例、scratch、选择、启停、参数、state、editor 与故障状态独立；global/track 操作不改变 input，反向亦然 |
| 切换 | 请求可在块边界接收，准备与尾音过渡独立验收；原生抑制与 overlay 一致提交，无重复监听、过期指针或 RSE 重建 |
| 宿主开关与空链 | 保存低延迟偏好不打开 LINE-IN；关闭 LINE-IN 后零输入处理且不抑制原生监听；空链或全部停用仍正常干声监听 |
| 输入故障 | 已激活时静音 input 并保留 GP output，不意外切到干声/原生监听；首次准备失败不改变原路由 |
| 稳定性 | 无音频线程分配、Qt 调用、阻塞锁、sequence gap、非有限样本和未解释的 xrun |
| 延迟证据 | 区分驱动 buffer、callback 时长、处理 CPU 耗时、插件延迟和实测监听延迟；后台计算 P50/P95/max，实测显示旁通消除了所定位的额外等待 |
| 回滚 | 关闭模式后解除分流并遵从 LINE-IN 当前状态，RSE/global/track 状态无需重建 |

## 7. 风险和明确边界

- 当前 hook 中的输入处理位于 GP 原始 callback 返回之后；如果更早发生聚合或 native effect 处理，需要经过模块 hash/prologue 门控的更早接入边界，且同时证明输出路径可达，不能用最终 output 后处理猜测或抵消。
- 静音 input 可能只停止新信号而保留效果计算与尾音，也可能影响录音、电平或宿主状态；它不是已实现的完整旁通。
- 驱动和宿主 block 大小可能不同。只取得 1024 帧时不能宣称直接获取了 128 帧；若未找到更早的合适边界，本模式不满足低 buffer 目标。
- VST3 自身的 lookahead、oversampling 或非零 `getLatencySamples()` 仍会增加输入监听延迟；本阶段记录并显示，不伪造零延迟保证。
- 低 buffer 缩短两路共用的处理期限；插件状态独立不能保证重负载下 RSE 完全免受音频超时影响。第三方插件兼容性与并发负载必须实测。
- 0.10.0 固定配置已取得硬件回环监听延迟对照；0.10.1 的六种 rate、九档 buffer 目前有算法与离线专项，完整硬件矩阵与安全模式下的物理延迟仍待测量；0.10.2 收尾时用户确认当前配置开启声卡驱动安全模式后爆音消失，编辑器重入崩溃已修复并回归。历史 128～4096 帧请求恢复 64 帧的记录来自通用控制接口主动回滚，不能推断 ASIO 驱动拒绝，也不证明其他 buffer 运行通过。Neural 离线 DSP 计时不包含完整宿主 callback 或驱动调度，不能替代物理 xrun 验收。

关联文档：[实时实现总览](REALTIME_IMPLEMENTATION_PLAN.md)、[运行逻辑与介入逻辑总图](PLUGIN_RUNTIME_AND_INTERVENTION.md)、[P12 事件驱动计划](P12_EVENT_DRIVEN_LAZY_RUNTIME_PLAN.md)、[测试与验证](TESTING.md)。
