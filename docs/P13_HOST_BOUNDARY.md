# P13-0 宿主边界调查

状态：宿主边界已用于普通构建的独立 input overlay，固定配置下已完成 ASIO 小块、监听分流与首次排空、混响尾音隔离、overlay/RSE 逐样本共存、设备拒绝/恢复及物理监听延迟验证（2026-09-19）。当前实现、发布验收和设备范围见 [P13 实现记录](P13_IMPLEMENTATION.md)。

本页保留早期反汇编和各轮观测的原始范围。历史记录中的未验证结论只约束当时证据，不代表当前普通构建仍未接入；生产使用须同时满足模块 hash、完整入口验证、固定代际生命周期和 SRC 拓扑合同，不能单独依靠某一偏移或旧快照。

## 当前边界与行为证据

- 普通构建已使用独立 listener sink 保留原函数的 capture、DSP 和状态更新，再按共享 output SRC/ring 的有限历史计数排空，最后将实际 192000 Hz ASIO capture 以 callback 原帧数送入 input VST3 并加到 GP output。GP/RSE 内部仍为 44100 Hz；只接受已核对的拓扑，其他配置 `host_limited`。
- `artifacts/p13-host-501b8cd9c26d495484843440276458e4/` 的稳态观测中，524288 个合成样本逐位等于原 GP output 加独立输入贡献；120422 个 output SRC 输入样本逐位等于 RSE unit 求和，差异和非有限样本均为 0。排空后 Ready 子窗口为 3697 块、108692 个 RSE 样本，双方能量非零。该轮 input 使用 unity fixture、手动 gain 0.5，parent gain=1；不外推到任意效果状态或切换时刻。
- `artifacts/p13-monitor-delay-comparison.json` 已取得同设备、192000 Hz/64 帧的同步 input1→监听输出→物理回环 input2 对照：原生 78.458333 ms，overlay 3.177083 ms，估计减少 75.28125 ms。它包含软件路径与 DAC/线/ADC，使用无 RSE 音符的专项曲谱，不能替代共存保真验收。
- `artifacts/p13-host-9083ebc10cc645799c2bb1a162a2c4d4/collection.json` 使用 production6，经正常 UI 将当前音轨音色模拟效果链 false→true，并在结束时恢复 false。原生监听和播放中 overlay 阶段分别对可见 `preGainVolumeSlider` 采样 12 次，均观察到动态非零 left，right 始终为 0，与本次单输入通道一致；每段首个样本可为 0，不能描述成每次采样都非零。该次 input 处理 26685 块、错误/削波为 0，配置恢复、正常退出、清理完成。这证明本配置下原生输入 UI 电平在 overlay 时仍更新；不声称所有输出 meter 或所有效果状态均已验收。
- `artifacts/p13-host-c348d9a1070f4d60b95a9a614e9fa656/` 补齐开启效果链、增益归零后的真实残留和首次切换观测：native pre-gain 在 4096 次 listener 调用中均为 0，sink 残留能量 17.478061；399 块排空之后才进入 overlay，生产与独立 drain state/phase 逐块一致，524288 个输出、120422 个 RSE/SRC 样本零差异。末端分流隔离持续尾音，后级有限排空隔离此前已经混入的缓存，不清共享 RSE DSP。
- 同轮 RTTI 确认 listener chain 内含独立 `E30_EqGEq`、`M07_DynamicClassicDynamic`、`M04_StudioReverbRoomAmbience`。`state + 0xD0` 的 `EXE + 0x25ABB20` vtable 已命名为 `I01_VolumeAndPan`（原先未知，非录音分支）。listener chain 的 Master 为空，RSE Master EQ/limiter/reverb/volume-pan 对象地址各自不同。原始字段与历史未命名记录留在下文。
- 最新普通构建 `p13-host-98199906536946a385991bd984df0121` 通过三 scope 独立参数/实例、切谱清链、播放中开关与一次确证 draining→Off 取消；Standard→ASIO 自动恢复。其他 rate/硬件保持未实测，不能从本机拓扑外推。

### 原生音频录音不适用的官方依据

[官方 GP8 用户指南入口](https://support.guitar-pro.com/hc/en-us/articles/5018404823069-GP8-Guitar-Pro-8-User-Guide) 链接的 [Guitar Pro 8 User Guide](https://static.guitar-pro.com/gp8/manual/Guitar-Pro-8-user-guide.pdf#page=315) 在 PDF 第 315 页（印刷第 309 页，Audio preferences）明确写道：

> There is not any record feature in Guitar Pro 8, but this access is mandatory.

前文说明麦克风权限用于 Tuner 与 Line-In。官方手册的 Audio Track 是导入音频文件，MIDI Input 则用于录入音符；两者不是原生音频录制。因此 P13 的“保留原生音频录音行为”在 GP8 中记为**宿主不提供／不适用**，不能写成已经运行并验证录音。实验 PCM recorder 同样不属于 GP 原生录音能力。

只读复核保存在 `artifacts/p13-recording-boundary/REVIEW.md`；334 页官方 PDF SHA-256 为 `f68b9e1626f8d4555c23cb19fb371efbf383b1157e76f7f8b099290af98127f7`，目标页已抽取文字并渲染核对。该结论来自明确官方说明，不是搜不到菜单的推断；Tuner 与其他实际输入消费者不因此免于其适用的行为验证。

## 模块与复现来源

早期静态调查文件位于 `C:\Program Files\Arobas Music\Guitar Pro 8`，使用 Visual Studio `dumpbin /disasm`、`dumpbin /exports` 和 PE 离线读取；该静态步骤未启动或操作用户宿主，后续独立测试进程另列运行记录。DLL 的版本资源没有返回版本字符串，版本归属依靠现有 `native/host_manifest.json` 的完整 SHA-256 匹配，不能把空版本资源写成独立版本证据。

| 文件 | SHA-256 |
|---|---|
| `AMAudio.dll` | `0151B8D484A0DBEDBB74AA1A43975932349F812A2D8929DFAAD149EFBD992394` |
| `GPRSE.dll` | `E983122951B94C2513A1F05828DD03DCB11620DDC50F6B497723CAE0EB32BA6A` |
| `GuitarPro.exe` | `B233B0F1C87DEB3AECE693D51E8D3C3A841C88FEE78828607B20034737C4C6DF` |

本地原始反汇编、导出表与调查脚本保存在忽略目录 `artifacts/p13-host-boundary/`，不进入发布包或提交。下面的地址均是对应模块的 RVA，不是运行时绝对地址。

`AMAudio.dll + 0x198AF0` 包含 `PortAudio V19.6.0-devel, revision 396fe4b6699ae929d3a685b3ef8a7e97396139a4`。函数命名与字段语义参考该版本的 [pa_asio.cpp](https://github.com/PortAudio/portaudio/blob/396fe4b6699ae929d3a685b3ef8a7e97396139a4/src/hostapi/asio/pa_asio.cpp)，字段偏移以本机 DLL 的指令为准；不能单靠上游源码假设本机二进制布局一致。

## ASIO 流与实际 block 的只读诊断

既有门控回调为 `AMAudio.dll + 0xABE0`，`userData` 是 PortAudio Impl。既有 ABI 先检查 `parent = *(userData + 0)`、`stream = *(userData + 8)`、`*(parent + 0x178) == userData` 和 stream magic `0x18273645`。

在匹配 hash、回调指针生命周期内，可增加以下验证：

1. `*(AMAudio + 0x2F2620)` 是 PortAudio ASIO 的 `theAsioStream`。只有非空且与当前 `stream` 相等，才按下表读取 ASIO 扩展字段；不匹配时不得将一般 PortAudio stream 当成 ASIO 结构。
2. `*(stream + 0x28) == userData`。`PaUtil_InitializeStreamRepresentation`（RVA `0x716C0`）在 `0x716E4` 将第 4 个参数 userData 写到 `+0x28`；ASIO 开流调用在 `0x6EB30`。
3. `stream + 0x10` 是 `PaUtilStreamInterface*`。ASIO callback interface 的 `Close`、`Start`、`Stop` 指针依次为模块内 RVA `0x6DF60`、`0x6FE90`、`0x700A0`；初始化见 `0x70DC8..0x70DDD` 和 `0x71660..0x716BC`。这些可作为额外交叉验证，不读取或调用未知指针。

| 观测值 | 字段/证据 | 能证明的范围 |
|---|---|---|
| ASIO 实际 host block | `uint32(stream + 0x178)` | 开流成功后保存的 `framesPerHostCallback`；`0x6ECC3..0x6ECD9` 向 ASIO `createBuffers` 传入请求块长，失败后的 preferred fallback 见 `0x6ECE8..0x6ED17`；成功值经 `0x6EEB1`/`0x6ECEF` 保存到局部，最终由 `0x6F795` 写入此字段。输入和输出 driver converter 在 `0x6D908`/`0x6DA58` 读取此字段作为帧数 |
| ASIO 报告输入/输出延迟 | `int32(stream + 0x190)` / `int32(stream + 0x194)` | `bufferSwitchTimeInfo` 在 `0x6D8A6`/`0x6D8C1` 读取并除以 `stream + 0x48` 的采样率；是驱动报告值，不是物理回环测量 |
| ASIO callback 入口 | `bufferSwitchTimeInfo` RVA `0x6D6C0`；旧式 `bufferSwitch` RVA `0x6D610` | ASIO callbacks 表在 `0x236540`，第 1/4 个指针分别是这两个入口；`0x6D6DA` 读取 `theAsioStream`，`0x6D944..0x6DA1F` 调用 PortAudio buffer processor，再转换 driver output；还需真实时间戳证明输出期限 |
| 宿主选中的 backend | Impl `+0x40028 == +0x40030` | `AudioLayer::isAsioEnabled` 导出 `0xE720` → `0x8670`。这是配置状态，不代替上述实际 ASIO stream 匹配 |
| UI bufferSize | Impl `+0x40070` | `AudioLayer::bufferSize` 导出 `0xE490` → `0x7B00`；`updateBufferSize` 的 `0xB396..0xB399` 会用 `PaAsio_GetAvailableBufferSizes` 返回的 preferred 值更新，不能直接当当前运行的 driver block |

ASIO 开流分支在 `0xA284` 将 `Pa_OpenStream` 的 `framesPerBuffer` 参数置 0（`paFramesPerBufferUnspecified`），在 `0xA29D` 调用 RVA `0x6CC60`。这说明该分支允许 PortAudio 按实际 host buffer 调用，**不证明本次设备正在运行，也不证明当前 callback 等于 driver block**；诊断必须分别记录两个数。

### 采样率陷阱

`stream + 0x48` 是传给 PortAudio 的 rate，不能单独当硬件输入的实际采样率。本机 GP 开流先使用 44100，而其修改版 `PaAsioStreamInfo` 还有 `+0x24` 扩展字段：GP 在 `0xA14C`/`0xA22B` 将其置 1；PortAudio 在 `0x6E966` 汇总该标志，`0x6EA2A` 非零时跳过设置 driver rate。该行为是本机二进制证据，不能由未修改的上游源码推导。

开流后，GP 在 `0xA3B7` 调用自己的 `PaAsio_GetSampleRate`（RVA `0x70930`），在 `0xA4EF` 将实际 rate 与 44100 比较；不同则在 `0xA5E7..0xA5F6` 创建 actual→44100 输入重采样器，在 `0xA63C..0xA64B` 创建 44100→actual 输出重采样器。因此 `frames / *(stream + 0x48)` 的 deadline 计算存在低估负载的风险。

历史 wrapper `int PaAsio_GetSampleRate(double*)` 经 `0x6A540` 调用 ASIO driver 的 `getSampleRate`（vtable `+0x68`），但会丢失 driver 错误码；当前控制线程直接调用已核对的 `0x6A540`，预置 NaN 并要求真实返回值为 0。查询与生命周期控制串行并绑定 generation/revision，不在实时 callback 添加 driver 调用，也不跨代复用旧值。尚未发现可直接在 callback 安全读取、保证为最新 driver rate 的独立字段。真实 callback 间隔只用于交叉检查，不能代替驱动查询；详见 [流生命周期](P13_STREAM_LIFECYCLE.md)。

## 原回调输出的可观测范围

`streamCallback` 在 `0xAD15..0xAD20` 取 `min(frames, 0x800)`。`0xAE16..0xAE57` 对内部输出 scratch（Impl `+0x20028`）清零；随后才调用 AudioLayer 的 buffer 填充处理（`0xAF14` 或 `0xB0CB`）。`0xB0CE..0xB0E5` 将 scratch 转换到传入的 output，`0xB0EA..0xB0F5` 调用 RVA `0x43AF0` 对 float32 输出限幅。

因此，诊断只允许在原回调成功返回之后、output 非空、格式/通道契约有效且 `0 < frames <= 2048` 时读取本块 `frames * outputChannels` 范围。不采集原回调前的 output，不读取超过原回调写入范围的尾部，不把非有限值当有效音频。上面的初始化证据只证明范围内可读，不证明处理正确、RSE 逐样本不变或真实扬声器发声。

原回调还包含可选输入重采样（Impl `+0x18`，调用点 `0xAEA0`）、可选输出重采样（Impl `+0x20`，调用点 `0xAF65`）和输出 ring（模块 RVA `0x270810` 附近，读写点 `0xAF90..0xB08E`）。本机固定配置的启用拓扑和动态计数已由后续联合记录验证，见 [排空分析](P13_SRC_DRAIN_ANALYSIS.md)；其他配置仍须运行核对，不能将 scratch 大小、ring 容量或单个 1024 常量当成聚合证据。

## 历史：输入监听隔离候选调查

原回调先把真实 input 经 RVA `0x43B50` 转入 Impl `+0x28`，再作为 `AudioLayerImpl` 虚函数 `+0x120` 的输入；该虚函数在本版本对应 RVA `0x127F0`，会把 input 传给当前 AudioUnit 集合，之后相加至 GP output。此入口仍是所有 input 消费者的上游，尚未证明是仅影响监听、保留输入电平/录音的隔离点。

`AudioLayer::inputLevel` 导出 RVA `0xE5F0` 读取 AudioLayerImpl `+0x28` 所属电平跟随器，getter RVA `0x31C10`。更新函数 `0x31C20` 的直接调用关系仍未找到，不能据此宣布静音输入候选保持了电平。后续 EXE 代码还显示监听单元自己维护另一组 peak 状态（见下），所以不能把此 AMAudio getter 未找到更新路径，当成 GP UI 输入电平不依赖 capture 的证据；两组状态的 UI 使用关系尚未确认。

`GPRSE.dll` 的 `SynthetizerAudioUnit::fillBuffer` RVA `0x8FB0` 是 sample player 预览分支，并不是此次枚举到的实际 RSE unit。此次真实 unit 的 RSE PCM 已在入环前经过 Master，EXE 监听单元随后单独处理 capture 并相加（见下）。这缩小了候选隔离点，但效果对象是否共享、其队列/尾音、录音及切换状态仍需验证，并需要可区分输入/RSE 的运行证据。

该调查阶段尚不满足 P13-0 退出条件，因而当时保持生产路径不变，继续收集真实流和隔离证据。后续普通构建采用更精确的 listener output sink，未采用上游静音 input 候选；当前实现和已补证据见本页开头。静音 input 仍仅为测试构建的候选实验，不能作为已发布旁通。

## 历史：第二轮短窗口运行证据

开发侧使用独立测试构建及副本曲谱运行的 `artifacts/p13-host-062bf4d0eacb49f99db4ada6498894e7/collection.json` 已完成采集，宿主 PID `55196` 正常退出（exit code 0），配置前后相同，原始曲谱 hash 不变。该采集不是上述只读反汇编操作的一部分。

原始采集文件 SHA-256 为 `4B94C2455C4D8BE0432996860E860B7DB069662FC5A743F2C99982091EE57EC2`；独立离线摘要见同目录 `probe-analysis.json`。

| 项目 | 实际观测 |
|---|---|
| 设备 | `ASIO` / `Midiplus USB Audio`；前后 UI 配置 buffer 64 |
| 流身份 | 512 条记录均通过实际 ASIO stream 识别，`host_api_type = 3` |
| driver / callback block | 两者均为 64 帧，共 512 条，约 170.1271 ms 的短窗口 |
| 采样率 | `PaStreamInfo` 请求值 44100；控制线程 `PaAsio_GetSampleRate` wrapper 返回 192000（wrapper result 0，不代表已保留 driver error） |
| 通道 | capture 1、output 2 |
| driver 报告延迟 | input 352、output 288 样本；不是物理监听延迟 |
| callback 序列/返回 | 窗口内无缺号/重复/时间戳倒退；status flags 与原回调返回值均为 0 |
| 短样本 | 每条 capture 与 post-original output 各采前 16 帧；均观测到非零有限样本，未出现 nonfinite mask |
| 原回调/完整观测段 CPU | 原回调 P50/P95/max 28.7/63.3/130.1 μs；观测段 29.7/66.6/130.2 μs（含 probe 开销，未含 publication 后工作） |

192000 的控制线程查询与 64 帧/约 0.333 ms 的 callback 节奏一致，揭示了“请求采样率 44100 不等于实际 capture rate”的实际情况。该查询发生在窗口之后，不证明每条历史记录的 rate 恒定；按该 rate 计算的预算比较只能写成有条件的短窗口结果。

这些数据支持本次配置下现有 hook 已收到 ASIO 64 帧块，不能继续假设 hook 总是等待 1024 帧。但这一轮不证明输入已绕过原生效果、不证明两个非零片段来自可独立辨别的 RSE 与实时输入、不证明连续播放或最终设备期限，也没有提供物理回环延迟、长尾、电平及 shared Master 非线性交互验收。当时 P13-0 尚未通过；后来的共存和延迟证据不能追溯成这一轮已经完成的内容。

## 历史：实际 AudioUnit 身份与运行时代码

后续独立测试宿主 PID `53472` 中，外部脚本于 UTC `2026-09-18T22:28:10.3625204Z` 读取 AudioLayer 当前 unit 集合；结果为 `artifacts/p13-host-boundary/units-53472-20260918222809765.json`，SHA-256 `902D3FC57A20646613DF5EDD41149DA0CAF4375873634BAEF73E03942B7B25F7`。脚本只使用 `OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION)` 和 `ReadProcessMemory`，不暂停、不注入、不修改宿主。

本次 parent vtable 为 `AMAudio + 0x18F7D8`，unit vector 有 4 项，边界前后一致：

| 索引 | 模块 | vtable RVA | fillBuffer RVA |
|---|---|---|---|
| 0、1、3 | `GPRSE.dll` | `0x23AD98` | `0x492B0` |
| 2 | `GuitarPro.exe` | `0x25ABAF8` | `0x4FDBD0` |

边界一致不等于获得 shared_ptr 所有权或暂停了销毁；该快照保留 `concurrent_object_lifetime_not_proven = true`。身份结果不是连续的生命周期合同，也不能单独证明本次播放已执行每条分支。

`GuitarPro.exe` 磁盘 PE 的 `.text` 和 `.rdata` 原始数据长度为 0，运行时才有代码与原 IAT 内容。因此 EXE 函数分析来自该次只读保存的 `unit-2-0x4FDBD0.bin`（4096 字节，SHA-256 `5CBFB738F6B10CF4F54B5E3EFB49E30C48B8EA8BD61A7F9DB4CE9252EB92E2E4`）；不能把磁盘零字节当函数内容。代码与 vtable 两个地址分别减其 RVA，均得到当次 EXE 基址 `0x7FF68AC80000`。保存的范围覆盖函数主体、返回和局部静态初始化分支（`0x4FDBD0..0x4FE609`）。

### RSE：Master 后的独立 PCM 队列

后续联合实验在同一 callback 中观察每个 GPRSE unit 的真实输出，按宿主顺序求和，
与 output SRC 的完整输入逐位比较。`51f02261a98d4c0888903124b5cbb183` 中 120422 个
float 样本相等，同时 listener sink 内仍有非零信号；具体范围见 [实现记录](P13_IMPLEMENTATION.md)。
AMAudio `0x102E0..0x10316` 会在每个 unit 调用前清零其独立 scratch，再由
`0x1034A..0x10470` 相加到 parent 输出。因此 listener caller 为全零是正常行为，
不能把它单独称为非零 RSE 音频。parent 在 `0x12B13..0x12BD3` 还有末端 output gain；
本次逐位求和比较只接受 gain=1、未静音的 parent，不外推到任意 gain。

`ConductorControllerImpl` 构造函数 `GPRSE + 0x433F0` 在 `0x4345F` 构造内部 unit（`0x43500`，vtable 写入点 `0x4351A`），在 `0x434AB` 用 `AudioLayer::playSound` 注册。与快照对应的 `fillBuffer + 0x492B0` 只保存 output 和 this，未保存或消费 capture 参数；其 `0x4955F` 调用 `0x49DA0`，从 `this + 0x198` 的 float PCM ring 复制到 output。事件分支与淡入淡出也操作该已有 PCM。

PCM 生产函数 `0x48570` 在 `0x487BD` 调用 `0x47890`，后者在 `0x47BA0` 调 `Conductor::fillBuffer`（`0x3DFE0`）。Conductor 在 `0x3E204` 执行 `Master::process`（`0xBDC90`），随后 `0x3E22F` 才把结果加入生产 buffer。生产者在 `0x48A09`/`0x48A1A`/`0x48A2E` 将该 buffer 复制至 `this + 0x1E8` 指向的 PCM 数组，在 `0x48A3B` 发布 `this + 0x198` 写位置；与消费端的读位置 `+0x1D8`、容量 `+0x1E0`、数据 `+0x1E8` 一致。

Master 的 limiter getter `0xBDC80` 返回 impl `+8`，该效果在 `0xBE224` 处理，发生在上述 PCM 发布之前。由此可静态确认本次 GPRSE unit 提供的是已经过 Master 的 RSE PCM，不能为了输入低延迟重排其生产/消费队列。这仍不排除 EXE 监听效果与其他对象共享实例或外部状态。

### EXE：capture、局部效果与末端相加

`GuitarPro.exe + 0x4FDBD0` 的参数与 AudioUnit 调用契约吻合：保存 capture `rdx`、output `r9`、this `rcx`，从栈读取 outputChannels 与 frames；以下 `state` 指 `*(this + 8)`。

| 阶段 | 指令证据 | 边界含义 |
|---|---|---|
| 总开关 | `0x4FDC88..0x4FDC92` 检查 `state + 0x50`；false 跳 `0x4FE4B0` 返回 | 跳过整个单元同时跳过电平和 DSP，不能直接当保留电平的监听隔离 |
| 输入转换与 peak | `0x4FDCB8` 导入调用接收 capture 和局部 scratch；`0x4FDCDD..0x4FDD2A` 搜索样本并更新 `state + 0x198` | peak 更新在输入 gain 与效果之前；邻近 getter `0x4FE670` 读取并清零此字段，但其 UI 调用者未确认。搜索用绝对值比较，写回用原始带符号样本与旧值比较，不能将字段解读为保证非负、有限的绝对峰值 |
| 输入增益 | `0x4FDD30..0x4FDD82` 以 `state + 0x58` 逐样本相乘 | 这是监听分支自己的增益 |
| 效果链 | `0x4FDD84..0x4FE02F` 取得 `state + 0xC0/+0xC8` 的 shared_ptr，在双通道局部 planar scratch 间交替处理；处理调用点 `0x4FDF38` 与 `0x4FDFB9`；`state + 0x54` 参与后续增益 | 第四轮 IAT 确认这是 `GPRSE::EffectsChain` 的 rail 0 效果及 channel strip，经 `AMOverloud::Effect::process` 处理 capture；开启时的对象归属仍待运行观测 |
| 可选后处理 | `state + 0xD0` 非空时 `0x4FE124` 处理局部 buffer；`state + 0x1A0` 非零时 `0x4FE254` 使用内嵌于 `state + 0x60` 的 `M06_DynamicAnalogDynamic` 对象处理，后接 `+0x158/+0x160/+0x168/+0x170/+0x190` ring 更新 | 此函数向 ring 写样本并扫描 peak，最终输出仍来自当前 scratch，没有从该 ring 取回音频供输出；后续复核将它确定为本函数的输出电平历史缓冲，不是监听延迟队列或录音队列。`+0x1A0` 是动态处理分支开关。`+0xD0` 是实际音频 DSP 分支，派生类仍未命名，不能猜作 Noise Gate 或仅 meter |
| 输出 peak 与最终相加 | `0x4FE335` 写 `state + 0x19C`；`0x4FE33E..0x4FE4AE` 把监听 scratch 加入传入 output；邻近 getter `0x4FE930` 读取并清零输出 peak | 此前的监听 DSP 操作局部 scratch，最终相加是目前最具体的候选分离边界；只跳此加法仍执行原生效果，不等于实现效果旁通 |

函数没有把传入 output 作为前段效果的输入，也未出现直接 `Master::process` 调用。第三轮保存范围不含运行时 IAT，第四轮补充的槽位目标已与导出表交叉匹配（见下）；仍不能仅凭函数名称证明效果没有共享状态。离线 PE 的重排 import 表不能可靠代替原槽位内容。

该末端相加还在 AMAudio 原回调的 output resampler 和全局 output ring 之前。即便在这里停止新监听样本，先前已进入这些缓存的监听音频仍可能继续输出；恢复时原生效果的内部尾音也可能存在。因此仅有上述静态分离边界不能推出“切换即时无旧尾音”。当前生产路径增加了保守计数排空，真实稳态共存与 UI 输入电平也已有独立证据；完整长尾和切换仍不可省略。

### 后续只读监听采样的 ABI 边界

同一 `IAudioUnit` 虚函数的导出修饰名（`GPRSE::SynthetizerAudioUnit::fillBuffer`）与 AMAudio `0x1031B..0x10347` 的真实调用共同给出 Windows x64 ABI：返回 `int64_t`；参数依次为 `this`、`const float* input`、`uint32_t inputChannels`、`float* output`、`uint32_t outputChannels`、`int64_t frames`、`const std::chrono::system_clock::time_point&`。诊断可将最后一项仅作为不解引用的 opaque pointer 原样转发。EXE 在 `0x4FE4B0` 返回传入 frames。

EXE 此版本前 12 字节为 `48 89 5C 24 18 55 56 57 41 54 41 55`，均为完整且不依赖指令地址的保存寄存器指令；下一条位于 `0x4FDBDC`。这仅是匹配 EXE hash、vtable、prologue 后实验 hook 的重定位依据，不能替代线程安全的安装/卸载与调用生命周期合同。

状态读取只可在该次已识别 unit 调用的借用范围内考虑，不能跨 callback 保存 `this` 或 `state`；应记录指针变化、未通过验证与 nonfinite 情形。邻近 getter 会清零 peak，跨线程读取观察到的变化不能一律解释成音频变小。当前 ASIO actual 192000 到 GP 44100 的重采样发生在 unit 调用之前，所以 unit frames 也不能直接当作 driver block，或者用 192000 为其计算处理预算；需要外层 callback 序列、调用次数和 unit 帧数的关联证据。

通道也需要分层记录：AMAudio `0xB1BF..0xB1D3` 初始化内部固定 float32、2 通道格式，`0xAE09` 把 driver capture 转入该格式，`0xAF0C`/`0xB0B7` 将其 channelCount 作为 unit 的 inputChannels 与 outputChannels。EXE 监听函数没有保存或使用传入的 inputChannels，局部转换按其双通道合同处理。因此本次 driver capture 1 通道不能直接当成内部监听单元 1 通道；只读 probe 应保存并验证本次 ABI 通道参数，而非复用 driver 参数读取 unit buffer。

## 历史第四轮：运行时 IAT 与关闭状态的监听回调

独立测试进程 PID `70328` 的身份文件为 `artifacts/p13-host-b5a97fa73931498988f0ecb13b85c842/audio-units-68befe842a6f494cb5b1a221edc32884/units.json`（UTC `2026-09-18T23:07:33.2757120Z`，SHA-256 `36CB12485C6D30C216570C0EAA11F9B763C7A3852638A74214610D1AEEC05473`）。本轮结果为 `partial_unvalidated`，不是隔离验收成功；空 Conductor/空监听链等字段明确保留未知或部分状态。

### 导入函数与对象身份

| EXE IAT 槽位 | 实际目标 | 对监听控制流的解释 |
|---|---|---|
| `0xE300F0` / `0xE300F8` | AMAudio `0x43B50 convertBuffer` / `0x43A40 AudioBufferFormat` 构造 | 输入/输出 scratch 格式转换 |
| `0xE300C0` / `0xE30170` | AMAudio `0x19440` / `0x194F0 AudioBuffer` 构造/析构 | 栈上 planar scratch |
| `0xE300C8` / `0xE300D0` | AMAudio `0x1E810 toInterleavedData` / `0x1E650 fromInterleavedData` | 在局部 planar/interleaved buffer 间转换；前者不是最终 `addToInterleavedData` |
| `0xE300D8` / `0xE300E0` | AMAudio `0x1E790 scale` / `0x1E380 data` | 本地增益与 planar 指针访问 |
| `0xE306A8` / `0xE306B0` | AMOverloud `0x158D00 Effect::bypass` / `0x15A2C0 Effect::process` | rail 效果只在未 bypass 时处理，channel strip 与后段效果使用同一 process wrapper |
| `0xE384D8` / `0xE384E0` / `0xE384E8` | GPRSE `0xB99B0 EffectsChain::effect` / `0xB99F0 effectCount` / `0xB97B0 channelStrip` | 监听遍历 rail 0，再处理 channel strip |
| `0xE384F0` | GPRSE `0xB97D0 EffectsChain::clone` | 邻近控制函数使用的链复制入口 |
| `0xE30668` / `0xE30670` / `0xE30678` | AMOverloud `M06_DynamicAnalogDynamic::getLimiterThreshold` / `getOutputGain` / `getExpanderThreshold` | `state + 0x60` 是带 limiter/expander 参数的动态处理器 |

本轮 `state = 0x2319AD84D30`，`state + 0x50 = 0`，`+0x1A0 = 1`，输入/输出 gain `+0x58/+0x54` 均为 0.5。监听 EffectsChain `+0xC0 = null`；`+0xD0` 指向 EXE vtable `0x25ABB20` 的效果，具体派生类尚未确定。内嵌动态处理器 vtable 为 `AMOverloud + 0x2BECA8`，导出名确认 `M06_DynamicAnalogDynamic`。

成功取得 Master 身份的 RSE unit，其 limiter 位于 `0x2319F098270`，监听内嵌动态处理器位于 `0x2319AD84D90`；二者是同类但不同对象。这个地址比较只能排除此次这两个外层对象完全相同，不能证明所有内部 DSP 状态或未开启的监听链均独立。

`AMOverloud::Effect::process` 在 `0x15A2F4` 调对象 vtable `+0x58`，随后可选地在 `0x15A30A` 调 `updateMeter`。因此“跳过 process 并仅保留 EXE peak”仍可能丢失效果自己的 meter，不能把它写成已证明保留全部电平语义的旁通实现。

### EffectsChain 复制的静态证据

捕获的 EXE 邻近控制函数 `0x4FE990` 在 `0x4FE9EA..0x4FEA12` 从 `state + 0x1A8` 取得源 chain，调用已映射的 `EffectsChain::clone`，再经 helper 赋给 `state + 0xC0`。GPRSE `clone` 在 `0xB97DD..0xB9806` 新建 chain 与 impl；`0xB67E0` 初始化独立的 0xD0 字节 impl，并逐一经 `EffectsFactory::clone`（调用点 `0xB691E`）复制 4 条 rail 的效果到 unique_ptr 集合。

复制 impl 的 `+0x88` 初始化为零，而普通 EffectsChain 构造函数 `0xB6A20` 在 `0xB6AAC` 将其设为传入的 `Master*`。复制流程不是直接共享源链的 Master 指针。`AMOverloud::EffectsFactory::clone`（`0x15DBA0`）经 `0x15DF30` 创建或从空闲池取出效果对象，然后在 `0x15DBE4` 调 `copyParameterFrom`；不能将它描述为每次全新分配或完整尾音复制。这些证据支持监听采用效果参数副本的设计，但本轮监听链为空，尚未运行验证开启/换轨后的实际复制及内部状态归属。

### 监听回调与原生 UI 入口

同轮 `data/p13-input-probe.json` 保存了 512 次 EXE listener 调用：输入/输出均为 2 通道，14 帧有 154 次、15 帧有 358 次，总计 7526 帧；512 个外层 callback 序列均不同，线程为 `53376`。所有调用都观测到 `unit_enabled_at_entry = false`、完整 frames 返回与 state 指针前后一致；输出调用前后均为零，输入/输出 peak 均为零，未见 nonfinite。这证明本轮在真实音频线程调用了监听单元，也验证了内部块长与 ASIO 64 帧不同；它没有执行开启后的原生监听效果，不能算旁通基线。

安装目录 `translations/GuitarProLocalisation_en.qm` 的原始 source/context 对提供了正常 UI 入口线索：`GPMenu / Activate line-in`、`LineInWidget / Line-in`、`LineInWidget / Line-in settings`，以及 `Effect Chain:`、`Input Gain:`、`Limiter:`、`Noise Gate:`、`Use sound simulation of the current track`。这些是静态 UI 文字线索，不是已观察到当前 QAction 可用。

现有 GuitarProMCP 已提供正常 Qt 操作：`gp_actions` 或 `gp_objects` 可用 `query = line` / `LineIn` 查询实际对象，再由 `gp_trigger(snapshot, id)` 调观察到的 `QAction::trigger` 或 `QAbstractButton::click`。快照只在最近一次查询后 60 秒内有效；`scheduled` 后必须重读 checked/控件和 listener 状态。后台禁用动作可先通过现有 `gp_window(state = restore)` 正常激活，不能伪写 enabled。开启可能出现宿主自身的 `Warning: Audio feedback` / `Warning: Latency`，应按实际对话框正常处理；本轮没有为此修改内存、设备设置或另起宿主。

最终监听混入是 EXE `0x4FE33E..0x4FE4AE` 内联加法，没有可供直接 TLS 限定禁用的 `addToInterleavedData` 调用。该阶段提出把 listener 的 output 参数暂时指向预分配、已清零 scratch，原函数仍调用一次并保留 capture、DSP 与状态更新；这成为后续 sink 实验和普通构建方案。它不节省原生 DSP 成本，单独也不证明全部副作用独立，仍需后级 SRC/ring 排空与真实音频证据。

### 正常开启监听的 UI 复核

后续独立进程 PID `2668` 的 `artifacts/p13-listener-ui-6c1d6e3db6ff4ef5930b4af0ef8dc81b/` 已通过上述正常 Qt 路径开启监听。`actions.json` 中 `actionActivatedLineIn` 起初 disabled/unchecked；打开副本曲谱并正常 restore 后，`restored-actions.json` 记录 enabled；触发后 `linein-objects.json`（SHA-256 `C2915EDBF7D555178CEC91EF84D6841400294C1390504FE44AD4643BC8A40A70`）记录 action 和 `lineInButton` checked。此次 `activation-dialogs.json` 没有活动对话框，未出现上述警告。

实际 Qt class 为 `gp::gui::LineInAudioUnit`（属性 `isLimiterEnabled = true`）、`gp::base::LineInFeature` 和 `gp::base::LineInModel`（`audioInput = 0`、`isEffectsBypassed = false`）。它们的查询结果未提供这些属性的 MCP 写入许可，因此这里仅把它们作为正常 UI 操作后的读回证据。

同进程 `audio-units-b0b8e911bffd4af7a84f8c66c7a0b054/units.json`（SHA-256 `35290A7196AEB2683B7E7661C2B1109951B1B973FE5881CCC99FE40E53AAE776`）与状态 dump 观测到 `state + 0x50 = 1`、`+0x1A0 = 1`，输入 peak 约 `0.0231964`、输出 peak 约 `0.00532702`。监听链 `+0xC0` 已非空，vtable 为 EXE `0x25ABB10`；`+0xD0` 仍为 EXE `0x25ABB20` 的派生效果。源链字段 `+0x1A8` 此时为零，因此不能把这个非空监听链直接解释为已复制当前曲谱音轨的效果链；尚需默认链类型、各 rail 对象和选择音轨模拟后的身份关系。

此轮确认了真实 QAction 控制入口、监听启用状态以及非零内部 peak。它不是逐块音频/RSE 对照测试，也未在当时验证物理延迟、正常 UI 电平或完整尾音合同；后续新增行为证据见本页当前状态，原生音频录音按官方能力记为不适用。
