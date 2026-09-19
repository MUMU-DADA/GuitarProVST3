# P13：共享输出 SRC 与 ring 的有限排空分析

本文记录 P13-0 的宿主分析及当前普通构建使用的切换算法。**计数状态机已经接入正式 overlay；本机固定配置的首次切换、混响尾音和 RSE 逐样本共存已实测通过。** `p13-host-c348d9a1070f4d60b95a9a614e9fa656` 从首个抑制 callback 连续比较生产与独立计数，399 块 preparing 后进入 active；原生前置增益归零后仍有残留，但不会进入输出。运行结果与设备范围见 [实现记录](P13_IMPLEMENTATION.md)，下文保留算法推导，不将静态证明等同于实机验收。

2026-09-20 更新：当前拓扑门禁已扩展到 44100、48000、88200、96000、176400、192000 Hz；原文第 3 节的 192000 Hz 限定是最初实现范围。新增拓扑、公式和真实 AMAudio SRC 验证见第 9 节。ASIO 驱动与长时间监听是否通过仍以实现记录中的独立实机证据为准。

分析针对本机 `AMAudio.dll` SHA256
`0151B8D484A0DBEDBB74AA1A43975932349F812A2D8929DFAAD149EFBD992394`，
下列地址均为该模块 RVA。完整片段保存在本地 `artifacts/p13-host-boundary/`；
该目录不进入提交或发布包。

## 1. 可行性与合同边界

EXE listener 的独立 DSP scratch 在末端才加到宿主输出。当前在该末端
混音前将输出分流到独占 sink，保留真实输入、原函数调用及返回值；native DSP
继续运行，输入 meter 的实际 UI 行为仍按独立证据验收。
低延迟 overlay 直接监听真实 capture，不等待该 native 路径。

GP8 [官方用户手册](https://static.guitar-pro.com/gp8/manual/Guitar-Pro-8-user-guide.pdf)
PDF 第 315 页（印刷第 309 页）明确不提供音频录制功能，因此计划中的原生
音频录制要求在本版本为宿主不提供/不适用，不将实验 PCM 采集算作录音验证。

[P13 计划](P13_LOW_LATENCY_ASIO_INPUT_PLAN.md) §3.1 和验收表要求排除原生
输入的干湿声、排队数据和尾音，并保留录音/电平；没有独立要求完全停止
native DSP 的 CPU 执行。末端抑制可以满足这项信号合同，但状态说明必须明确：
**原生监听贡献旁通，原生 DSP 为维护宿主语义继续运行。**
不能宣传 native 效果计算已停止或 CPU 已降低。

原监听贡献可能已经进入共享 output SRC/ring。不能立即开启 overlay，不能
清空共享状态，也不能在混音后估计或抵消旧贡献。当前过渡是：

1. input 实例、格式、rate、generation 和宿主身份全部准备成功。
2. 块边界发布单份 `preparing/draining` 快照：抑制 listener 末端贡献，overlay 暂不输出。
3. RSE 与原 SRC/ring 照常运行，按以下可证明的计数排空旧输入贡献。
4. 在排空完成后的下一块，用同一快照启用 overlay，继续抑制 native 贡献。

这包含一次切换期间的监听空隙，不增加稳定 overlay 的 FIFO。计划中的
“同时控制/一致提交”需明确包含上述准备过渡，不能把 `draining` 显示为 `active`。
若要求首次抑制的同一块立即产生 overlay，同时严格排除已存在的历史贡献，
则非空 SRC/ring 下不满足该组合要求。

## 2. SRC 的已定位结构

`StreamSampleRateConverter` 构造 `0x516B0` 建立两个每声道 resampler，
位于 impl+0 / impl+`0x78`；对象首指针为 impl。实际算法为内嵌 r8brain。
上游 `version-1.6`，commit `ace65a520b4d135e3e8eddeee8dccdbc4366e8bb`，
与二进制的 8 个 convolver 槽、virtual 布局、模板类型、实例大小和处理控制流匹配。
这不是对构建源码 commit 的认定。参考源文件：

- [CDSPResampler.h](https://github.com/avaneev/r8brain-free-src/blob/ace65a520b4d135e3e8eddeee8dccdbc4366e8bb/CDSPResampler.h)
- [CDSPBlockConvolver.h](https://github.com/avaneev/r8brain-free-src/blob/ace65a520b4d135e3e8eddeee8dccdbc4366e8bb/CDSPBlockConvolver.h)
- [CDSPFracInterpolator.h](https://github.com/avaneev/r8brain-free-src/blob/ace65a520b4d135e3e8eddeee8dccdbc4366e8bb/CDSPFracInterpolator.h)

构造参数为 MaxInLen=`32768`、transition band=`2.0`、attenuation=`180.15`、
linear phase、UsePower2=true。主 process=`0x50200`，convolver process=`0x502D0`，
interpolator process=`0x4FA90`。

convolver 在 `0x50509/0x50523` 复制/保存原始 input history，随后才 FFT，
在 `0x50917/0x5091B` 交换输入/输出工作区；没有输出反馈到 input history。
interpolator 用 256-sample ring 和 24-tap FIR，只推进读写位置，不反馈输出。
因此这是有限历史，不是无限衰减的 IIR。

`frameCountNeededBeforeOutputStart` 仅逆序组合启动需要量。
`0x4F630` 对 convolver 计算 `(InputLen-InputDelay+NextInLen*DownFactor)/UpFactor`；
`0x4F5A0` 对 interpolator 返回 `NextInLen+12`。**这些值不等于 flush 上限。**

## 3. 第一项可执行候选的前置条件

以下保守计数仅覆盖**经实例确认的 44100→192000 output SRC**，其每声道结构
应为一层 `UpFactor=2, DownFactor=1` convolver，后接 `88200→192000`
interpolator。不能把结论直接外推到别的采样率、多级结构、mono/stereo 变更或
其他宿主版本。

控制线程可预读不可变结构字段，但 callback 使用它们前必须验证同一 generation
的对象身份。回调中的动态计数必须属于该 callback 的一致快照；远程进程读取
不能代替这项保证。

| 对象 | 必须核对的字段 |
| --- | --- |
| resampler | +0x48 ConvCount=1；+0x08 Convs[0]；+0x50 Interp；预期 vtable |
| convolver | +0x28 UpFactor=2；+0x2C DownFactor=1；+0x30 DoConsumeLatency=true |
| convolver | +0x34 BlockLen2；+0x3C PrevInputLen；+0x40 InputLen；+0x44 Latency |
| convolver | +0x58 InputDelay=0；+0x50 UpShift=1；+0x54 DownShift=0 |
| convolver | +0x80 InDataLeft；+0x84 LatencyLeft，均在合法范围 |
| filter=*(convolver+8) | +0x48 KernelLen；+0x4C BlockLenBits，和上述长度一致 |
| interpolator | +0x1008 SrcRate=88200；+0x1010 DstRate=192000；预期 vtable |
| interpolator | +0x1020 BufLeft、+0x1024 WritePos、+0x1028 ReadPos 在合法范围 |
| stream | 正常处理中的 rate/channel/generation 不变；SRC 不被其他线程 clear/reconfigure |

定义每个声道：`I=InputLen`，`B=I/2`，`P=PrevInputLen`，`L=Latency`。
要求 `I>0` 且为偶数、`0<=P<=B`、`B+P=BlockLen2/2`、
`0<=LatencyLeft<=L`，所有算术有界且无溢出。
左右声道分别验证，门限取两者最大值。

联合实验已逐块核对真实实例：两声道 InputLen=2680、PrevInputLen=708、
BlockLen2=4096、Latency=3388、KernelLen=1417；因此 A=4020、T=1822 个源帧。
这些值只适用于该实例拓扑，其他 rate/结构必须重新验证。

## 4. SRC 的两段保守计数

计数单位为**实际交给这个 output SRC 的 44100 Hz frames**。不能累计硬件 callback
frames；output ring 充足时 `0xAEE5` 会跳过 parent 和 output SRC。
必须观察 `0xAF65` 的实际调用及其对象、输入 frames、返回 output frames。

### A：排除 convolver 的旧依赖

从 listener 末端抑制生效后的第一项 output SRC 调用开始，累计实际输入 frames，
直到一个完整 process 返回后的累计量达到 `A = max(3*B_left, 3*B_right)`。
跨越门限的整个调用仍视作可能含旧贡献。阶段 B 只能从下一项 process 调用开始。

三个块的保守证明：

1. 最多 B 个干净输入使当前部分填写的块完成；其 output 和保存的尾部仍可能含旧输入。
2. 再 B 个干净输入组成完整干净的新块；旧 history 仍可影响这次 FFT，但新的
   PrevInput 已全部干净，因为 `P<=B`。
3. 再 B 个干净输入产生完全干净的 FFT output，并完成与旧 CurOutput 的交换。
   本块之前发出的 output 仍可能含旧输入，交换后的所有未来 output 不再依赖旧输入。

这里“干净”表示不依赖被抑制的 native 监听贡献；RSE 信号可以持续存在，不需要喂零。
相位/部分块的最坏情况已经由第一个 B 覆盖，不需要猜测当前 FFT 进度。

### B：覆盖 interpolator 的全部历史

阶段 A 返回后，convolver 今后的 output 已干净。对于 `UpFactor=2, DownFactor=1`，
在 n 个后续输入内，它向 interpolator 提供的样本数至少为 `max(0,2*n-L)`；
L 仅作为最坏启动丢弃量，即使实际 LatencyLeft 已归零仍使用该保守值。
`copyToOutput` 的 down=1 分支直接复制，block phase 不再另加输出聚合。

因此阶段 B 门限可取：

```text
T = max(ceil((256 + L_left) / 2), ceil((256 + L_right) / 2))
```

从下一项 output SRC 调用起单独累计输入 frames，完整 process 返回后达到 T，
两个 interpolator 的 256 个物理 ring 位置都已至少被干净样本覆盖一次。
阶段 B 跨越门限的调用仍可能在前半部分返回旧数据；**只有后续 process 的全部
返回结果可标记为干净**。interpolator 的 phase 只依赖 rate/count，不依赖样本值。

这是一项可执行的有界计数规则，已由普通构建的独立状态机执行。若每次实际 SRC 输入满足 `0<n<=M`，
两阶段完成前总输入量小于 `A+T+2*M`。M 必须来自被验证且执行门控的调用容量；
`maxFrameCount=32768` 可作最大合法容量，不能冒充实际块长。
零帧调用不推进计数；设备暂停/停止也不形成成功证据。不存在无条件的墙钟完成上限。

## 5. ring 的旧数据边界

ring base=`0x270810`，capacity +0、mask +8、count +0x10、data +0x18、
read index +0x30、write index +0x38。`0xB178→0x5C80` 构造容量为
32768 **float samples**；stereo 最大 16384 frames。它是存储容量，不是测得的延迟。

在阶段 B 达标的那次**整个宿主 callback 返回后**，取 ring 当前 queued count 为 Q。
这时跨越门限 SRC 调用的输出已经追加并被部分消费；将余下 Q 个样本全部标为可能旧。
后续追加的数据才保证干净。不得在 SRC 刚返回、旧输出尚未追加时读取 Q。

后续每次 callback 观察：

```text
q_before = callback 前 queued samples
appended = 本次已验证 output SRC 返回 frames * channels，未调用则为 0
consumed = min(q_before + appended, callback_frames * channels)
q_after  必须等于 q_before + appended - consumed
old_remaining = max(0, old_remaining - consumed)
```

要求 count、channels、frames、返回量均在范围内；实际写入前的
`q_before+appended` 不超过容量；没有其他 producer/consumer；回调串行；generation
和 ring 对象不变。wrapped read/write index 本身不足以区别空/满或跨圈，不能单独计数。

`old_remaining==0` 后，ring 不再含旧 native 贡献。完成这一条件的 callback 的
设备 output 仍可能包含刚消费的旧数据，因此只在**下一次 callback 入口**提交
overlay active。Q=0 时同样在下一块提交。开启 overlay 后不得继续添加延迟以等待 native。

## 6. 失效、取消与反向切换

- 首次插件准备失败发生在开始抑制之前，保持原路由。
- drain 期间 generation、身份、计数不变量或 DSP 拓扑不满足时，不能按超时强制 active；
  清除待提交状态并报告具体原因。恢复还是保留抑制必须按该流已经验证的路由规则处理。
- 用户取消时在同一块快照关闭待激活 overlay 并恢复原先 native 监听状态。
  native DSP 一直在运行，可以恢复它的末端贡献；重新通过 SRC 的监听仍有原生路径等待。
- active→off 时先在块入口停止直接 overlay，再允许原 native 末端贡献。
  不需要清共享 SRC，不得把 input VST3 尾音继续叠加到已恢复的 native 监听。
- native 开关始终由宿主和用户控制，overlay 不写它。关闭 overlay 只解除末端分流，遵循用户此时的 native 状态；不会把激活期间的用户修改覆盖为旧值。尾音专项覆盖 native gain 的正常 UI 修改；原生监听开关与 overlay 的组合验证见实现记录。
- 无论处于哪个过渡，native 抑制、overlay 输出、slot、route 与 generation 仍只使用
  一份 callback 快照，不能在回调中读取互相独立更新的开关。

## 7. 已做检查与尚缺验证

本地 `check_src_drain_model.py` 用保守依赖标记验证了 91520 个 convolver
块长/历史长/初始部分块组合、256 个 interpolator 写位置、1040 个 ring 边界组合。
模型将含任何旧输入的 FFT output 整块视为旧数据，验证三块规则与 ring 边界规则。
它不执行宿主、不执行真实 FFT、不证明采样值或录音语义，也不是功能验收。

最终首次切换观测从首个 suppressed callback 开始，生产与独立 tracker 的
drain state/phase 逐块一致，排空期间 GP output 不改写，Ready 后才叠加 input。
原生混响尾音专项在前置 gain 为 0 时确认 sink 持续收到残留，RSE/SRC 与输出
逐样本比较无差异。可变块、左右声道、underflow、计数异常与生命周期有离线
专项；快速取消、Standard/ASIO 切换和恢复有实机证据。证据编号与范围见
[P13 实现记录](P13_IMPLEMENTATION.md)。其他硬件、实际采样率及未开放的 SRC
拓扑仍未实测，不将固定配置结果扩大到这些范围。

## 8. 独立排空状态机 API

实现位于 [input_drain.h](../native/modules/input_drain.h)，命名空间为
`gpvst3::input::drain`，普通构建由 `MonitorCallback` 驱动；仅观测和导出部分
保留在实验构建中。`Tracker` 使用固定字段，无分配、锁、宿主读取或时钟。
所有方法由同一个线程串行调用；跨线程读取须由外部发布 `Snapshot` 副本。

1. `configure(Config)` 只调用一次。Config 包含非零 epoch、声道数、固定源/目标
   rate、容量，以及每声道经过核对的 convolver/interpolator 结构参数。
2. 每次 `beginCallback(observedConfig, sequence, actualDeviceFrames, queuedBefore,
   nativeSuppressed)`，检查实际配置、连续序号、ring 连续性和该块监听抑制状态。
   只有这里能从 `preparing` 进入 `ready`，且必须晚于排空完成那块。
3. 如果宿主实际调用 output SRC，调用一次
   `srcCompleted(actualInputFrames, returnedOutputFrames)`；不得填名义 block。
   根据此二进制的队列条件，缺少本应发生的 SRC 调用或多出调用均视为失效。
4. 原宿主 callback 完整返回后调用
   `endCallback(observedConfig, queuedAfter, actualConsumedSamples)`。
   消费量是独立观察证据，必须同时满足 ring 前后等式与实际 callback 请求。
5. `snapshot()` 返回 `invalid/preparing/ready`、当前阶段、错误原因、门限与进度。
   `invalid` 为终态；重试、流重建或新请求必须创建新 Tracker。

该对象只验证计数证据，不会抑制声音或开启 overlay。`observedConfig` 必须来自
真实当前对象，不能简单回传最初 Config 冒充核对。计数值可能仍有伪造/采集错误，
宿主 hook 的身份、线程和数据来源验证不能由此对象替代。

专项脚本 [test-p13-drain.ps1](../native/test/test-p13-drain.ps1) 已通过
MSVC `/W4 /WX` 编译与执行，覆盖不等声道门限、跨门限整块保守处理、零进度、
队列不足/充足、真实旧 frontier、配置/epoch 改变、序号溢出、算术边界、
错误终态和回调无分配；PowerShell 语法检查通过。测试没有运行真实宿主或测量音频。

## 9. 多采样率拓扑与真实 SRC 验证

当前实现仍读取当前实例，核对同一 hash 的对象、vtable、长度、系数、动态边界、
stream generation 与 rate revision，不把目标采样率列表代替实际拓扑检查。

| 实际设备采样率 | GP 输出 SRC 结构 | 排空阶段 |
| --- | --- | --- |
| 44100 | 不创建 output SRC，不经过共享 ring | 当前 callback 开始抑制后可直接输出；不存在 SRC 历史 |
| 48000、96000、192000 | 一个 2× convolver，后接 88200→实际 rate interpolator | 保留 `A=3B1`、`T=ceil((256+L1)/2)` 与旧 ring frontier |
| 88200 | 一个 2× convolver，无 interpolator | `A=3B1`，该调用结束后记录旧 ring frontier |
| 176400 | 两个 2× convolver，无 interpolator | `A=3B1`，随后 `T=ceil((3B2+L1)/2)`，再记录旧 ring frontier |

176400 Hz 第二阶段复用三块历史规则：第一阶段结束后，第一个 convolver 只提供
不含旧监听的输出。再输入 n 个源帧，至少产生 `max(0,2n-L1)` 个干净样本；
覆盖第二级所需的 `3B2` 即可排除第二级的旧输入与旧输出。跨门限调用的全部
返回值仍视作旧数据，只有后续返回值干净。第二级对象、滤波器、fractional latency
或长度变化都会使拓扑连续性失效。

[test-p13-native-src.ps1](../native/test/test-p13-native-src.ps1) 在独立进程加载
匹配 hash 的真实 AMAudio.dll，调用真实 SRC 构造/处理/销毁函数，不开启 GP、
ASIO 设备或用户数据。对两个相同进度但旧输入不同的 SRC，在切换后输入相同
信号，按实际实例计算门限后比较输出。2026-09-20 实测：

| rate | convolver 层数 | A / T（44100 Hz 源帧） | 门限后比较样本 | 最大差异 |
| --- | --- | --- | --- | --- |
| 48000 | 1 | 4020 / 1822 | 442682 | 0 |
| 88200 | 1 | 4020 / 0 | 823992 | 0 |
| 96000 | 1 | 4020 / 1822 | 885364 | 0 |
| 176400 | 2 | 4020 / 1825 | 1626856 | 0 |
| 192000 | 1 | 4020 / 1822 | 1770728 | 0 |

该测试执行真实 FFT/SRC，验证已扩展拓扑及排空门限，不验证实际 ASIO 驱动、
监听延迟、VST3 CPU 预算或长时间无爆音。44100 的无 SRC 路径、真实 ring
消费计数、两阶段边界与对象变更仍由 drain/drain-probe 专项分别覆盖。

[test-p13-callback-split.ps1](../native/test/test-p13-callback-split.ps1) 直接调用
生产 `streamCallbackHook`，通过合成宿主元数据与受保护的输入/输出数组验证
6 种采样率 × 9 种 buffer × 3 种声道映射。覆盖 4096/8192 buffer 按 2048
分段时每一帧只读写一次、ADC/DAC 采样位置偏移且保持 `currentTime`、边界外不写、Start 期间 rate 尚未验证、
capture 为空，以及原回调提前结束时尾部清零，均已通过。这里替代的是原宿主
处理函数，因此不将该测试当作真实设备的音频通过结论。
