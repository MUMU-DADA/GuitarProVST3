# P13 ASIO 流生命周期与通道身份调查

状态：生命周期 hook、固定代际 callback 代理、rate revision 与 Close/Dispose 退役保护已接入普通构建；固定 ASIO 配置、普通构建 Standard→ASIO 自动恢复及离线 rate/reset/释放故障专项已通过。其他硬件、真实 rate 改变及异常 driver 通知仍未实测，按宿主受限范围记录（2026-09-19）。当前验收证据见 [P13 实现记录](P13_IMPLEMENTATION.md)。

本文件保留早期静态调查的 RVA 和 ABI 推导，另明确当前实现。旧 `writeJump` 和“普通构建禁止 P13”属于历史状态；当前普通构建链接严格 MinHook 和生命周期模块，但只在已验证的宿主、192000→44100 输入及对应输出 SRC 拓扑上允许 overlay。

## 证据范围

- 目标为 Guitar Pro 8.1.1.17 的 `AMAudio.dll`，SHA-256 为 `0151B8D484A0DBEDBB74AA1A43975932349F812A2D8929DFAAD149EFBD992394`。
- 离线反汇编为 `artifacts/p13-host-boundary/AMAudio.disasm.txt`，SHA-256 为 `52A5BE54EEBB3F258D413B1B027AD5C8D695245AD29D935A3D4E30BC0D7E4D7A`。
- 结构命名参考嵌入版本 PortAudio `396fe4b6699ae929d3a685b3ef8a7e97396139a4` 的 `pa_asio.cpp`；下面的偏移、分支和字节以本机二进制为准。
- 所有地址均为 AMAudio 模块 RVA。模块 hash、函数表、完整指令边界和当前流身份必须同时匹配；只有某个指针非空不能建立生命周期。

关联：[宿主边界](P13_HOST_BOUNDARY.md)、[SRC 排空](P13_SRC_DRAIN_ANALYSIS.md)、[计划门禁](P13_LOW_LATENCY_ASIO_INPUT_PLAN.md)。

## Start、Stop、Abort 和 Close

四个函数均为 Windows x64 ABI：`int32_t(void* stream)`，`RCX` 为 `PaAsioStream*`，`EAX` 返回 PortAudio error。它们不是 C++ 成员函数，不额外传递 `this`。返回 0 仅表示对应函数的成功结果，不能单独当作 callback 已全部退役。

`PaUtil_InitializeStreamInterface`（`0x71660`）把 `Close/Start/Stop/Abort` 分别写到接口表 `+0/+8/+0x10/+0x18`。ASIO 初始化 `0x70DBC..0x70DDD` 和 blocking 接口初始化 `0x70E34..0x70E4E` 都使用下列同一组函数。运行时流的接口指针位于 `stream+0x10`。

| 函数 | RVA | 可完整复制的最短不少于 12 字节的入口 | 长度 | 恢复地址 |
|---|---|---|---|---|
| Close | `0x6DF60` | `40 53 48 83 EC 20 48 8B D9 48 83 C1 68` | 13 | `0x6DF6D` |
| Start | `0x6FE90` | `40 55 56 41 56 48 83 EC 40 45 33 F6` | 12 | `0x6FE9C` |
| Stop | `0x700A0` | `48 89 5C 24 10 56 48 83 EC 20 48 8B D9` | 13 | `0x700AD` |
| Abort | `0x6DCB0` | `48 89 5C 24 10 56 48 83 EC 20 48 89 7C 24 30` | 15 | `0x6DCBF` |

上述范围没有 RIP-relative、相对 call 或跳转。历史 `writeJump` 使用 12 字节绝对跳转且未暂停执行线程，仅凭这些 prologue 不能安全热补丁；该实现已删除。当前指令 hook 使用 MinHook 的完整指令解码与 trampoline，并通过本地严格 enable/disable 扩展暂停线程、核对冻结后的线程集合和指令指针；线程位于补丁区时拒绝本次操作，不移动 IP。匹配字节仍是版本门控，不替代安装并发保护。

### Start 返回之前已经可能有 callback

Start 在 `0x6FEED` 将 `stream+0x210` 的 `reenterCount` 置为 -1，重置完成事件与处理器；在 `0x7003D/0x70044` 设置 stopped=0、active=1，再于 `0x70055` 调用 ASIO start thunk `0x6A6C0`。该 thunk 直接转驱动 vtable `+0x38`。

驱动在 start 调用期间即可回调。因此必须在调用原 Start 前建立当前 generation 的准备态并推进 revision；在 Start 返回 0、实际 rate、通道映射、slot 与 native 抑制合同全部有效以后，才允许 overlay 在后续完整块边界激活。当前 generation 在 CreateBuffers 时建立，不能等到 Start 返回后才首次绑定 callback。

### Stop/Abort 成功也不是无限等待合同

Stop 在 `0x7015F`、Abort 在 `0x6DCCB` 调 `0x6A6E0`，该 thunk 直接转驱动 vtable `+0x40`。两者在驱动 stop 成功后，最多循环 2000 次 `Sleep(1)` 检查 `stream+0x210 == -1`：

- Stop：`0x7016B..0x7019A`。
- Abort：`0x6DCD7..0x6DD01`。

循环用尽后仍走设置 stopped=1、active=0 的尾部；未观察到此分支把等待耗尽转换为独立 error。因此仅检查返回 0、active=0、stopped=1 都不足以证明 callback 已退出。Stop 还存在等待播放完成/阻塞输出的前置逻辑，不能在原 Stop 调用前阻止全部原 callback，否则可能破坏宿主的 drain/完成事件。

### Close 先释放流，再清全局指针

Close 在 `0x6DF75` 调 `0x716F0` 清 stream magic；随后释放 processor、事件、blocking state 和通道表。`0x6E061` 释放 stream 本身，`0x6E066/0x6E06B` 释放 ASIO buffers/driver，最后才在 `0x6E072` 把 `theAsioStream`（模块 `+0x2F2620`）置空。

因此控制线程执行 `if (theAsioStream) read(...)` 或 `if (theAsioStream) getSampleRate(...)` 存在关闭窗口，且先后两次读指针相等仍不能排除释放后地址重用。Close hook 的进入事件必须先使插件 generation 无效并封闭插件借用入口，不能等原 Close 返回后再失效旧状态；原 Close 返回后禁止再次解引用旧 stream。

## callback 退役边界

外层 ASIO `bufferSwitchTimeInfo` 为 `0x6D6C0`，ABI 为 `ASIOTime*(ASIOTime*, int32_t doubleBufferIndex, int32_t directProcess)`。旧式 `bufferSwitch` 为 `0x6D610`，ABI 为 `void(int32_t doubleBufferIndex, int32_t directProcess)`，转入 time-info 路径。GP 原 callback `0xABE0` 是其下层，返回后外层还会执行 driver output converter、`outputReady` 和 stream 状态更新。

外层 `0x6D6DA` 先取 `theAsioStream`，`0x6D6F0` 原子增加 `reenterCount`；原值 -1 的调用进入处理，其余调用记录重入并返回。主调用在 `0x6DB54` 原子递减并可能继续处理积累的回调。这个机制不能当作插件自己的引用计数，更不能从控制线程直接改写它。

只在 `streamCallbackHook` 引用计数，足够限定插件在该次 hook 中对自己 slot 的借用，但不足以证明整个 ASIO callback 已返回。正式保护原 stream 释放需覆盖外层 callback，或者通过可证明的宿主/驱动 stop 合同获得同等保证。

外层入口前 11 字节是两次 push 和 `sub rsp,0x98`，第 12 字节已进入 RIP-relative security-cookie load（`0x6D6CB..0x6D6D1`）。早期直接复制的 12 字节 trampoline 不能用在此入口；即使重定位到完整 18 字节边界，旧 `writeJump` 的 `mov rax, destination; jmp rax` 还会覆盖刚载入 RAX 的 security cookie，令后续 `xor rax,rsp` 使用错误值。当前采用 CreateBuffers 时验证并包装完整 callback 表的固定代际代理，不在这条外层入口复制或截断指令；旧 helper 已删除。

旧式 `bufferSwitch` 在 `0x6D675` 先调用 driver `getSamplePosition` thunk `0x6A510`，再在 `0x6D696` 转入 time-info 函数。因此仅保护 `0x6D6C0` 会遗漏旧式入口前段的 driver 生命周期；需要同时覆盖两个 ASIO 入口，且嵌套转调用不能重复准入或在中途被错误拒绝。旧式入口的前 15 字节 `4C 8B DC 49 89 5B 10 57 48 81 EC D0 00 00 00`、恢复地址 `0x6D61F` 仅保留作历史指令分析；当前两种入口均由 callback 表代理覆盖，原函数内部互调不再次进入代理。

## 实际采样率查询的错误码

`PaAsio_GetSampleRate` wrapper `0x70930` 并不保留 driver result：

1. `0x70939` 将未初始化的栈上 double 地址传给 `0x6A540`。
2. `0x70943` 直接读取该 double，不检查 `EAX`。
3. 只有 double 与 0 相等才返回 -9999；非零或 NaN 经 `0x7095F` 写回 caller，`0x70963` 强制返回 0。

因此既有 probe 中 wrapper 返回 0 且值为 192000，只能记录“wrapper 观测值”。callback 间隔提供一致性支持，但 wrapper 的成功结果不能证明 driver 查询成功，更不能证明整个历史采样窗口属于同一 rate generation。

更严格的只读入口为 `0x6A540`，ABI 是 `int32_t(double* rate)`：把 caller 的 `RCX` 移至 `RDX`，读取 `IASIO*`（模块 `+0x2F21B8`），空指针返回 `ASE_NotPresent=-1000`，否则直接 tail-call driver vtable `+0x68`（`getSampleRate`），原样保留 driver `ASIOError`。

当前控制线程查询要求同时成立：

- 持有与 Start/Stop/Abort/Close 串行的生命周期借用，不能只有裸指针检查；不在音频 callback 调 driver。
- 调用前将 double 初始化为 quiet NaN，要求返回值恰为 `ASE_OK=0`，值 finite 且在项目支持范围内；保留原始 error，不把任何正数当成功。
- 查询前后的 generation、stream、owner、driver identity 均相同，且 generation 未被 reset/rate-change 事件失效。
- 把值及其 generation 一起发布；失效后立即停止使用该值准备或运行 input processor。

该 thunk 的 ABI/错误转发已静态确认，真实记录取得 `ASIOError=0`、192000 Hz。当前普通构建使用非实时控制锁串行 query 与 Start/Stop/Abort/Close，并在 callback 验证 generation/revision；真正采样率变更、驱动故障和超时路径仍缺完整硬件验收。

## stream 不变也可能改变 rate

ASIO callbacks 表为模块 `+0x236540`：bufferSwitch、sampleRateDidChange、asioMessage、bufferSwitchTimeInfo 各占 8 字节。Open 在 `0x6E788` 先把 sample-rate callback 槽（`+0x236548`）写为默认 `0x6DBC0`，但这不是最终值：随后 `0x6E848..0x6E853` 和 `0x6E954..0x6E95F` 从输入/输出 `PaAsioStreamInfo +0x18` 取得非空自定义入口覆盖它。GP 在 `0xA21D..0xA224` 写入 `0x9850`，该入口忽略传入 rate 并转 Qt 重建调度器 `0x97A0`。真实 createBuffers 观测 `artifacts/p13-host-225fef6de9544d7aaf14ab156de75ff2` 确认最终四项分别为 `0x6D610 / 0x9850 / 0x6DBD0 / 0x6D6C0`。此前“当前流只注册空 rate callback”的推断不成立；只在某次初始化时改表仍会被下次 Open 覆盖。

`asioMessage` 为 `0x6DBD0`，把 message 转交模块 `+0x2F2628` 的 GP handler；GP 在开流设置的 handler 同样为 `0x9850`，跳 `0x97A0`。reset selector 3 和真实注册的 rate-change 回调都会调度 Qt 重建；动态触发后的具体时序仍需单独测试。

当前四项 callback 均按代际包装；rate-change 与 reset/buffer-change/resync 通知立即推进 revision，使旧 rate/slot/drain 资格失效，原通知仍转交 GP。实时线程只做原子标记，控制线程重新查询和准备。是否所有实际 driver 变更都发出通知还需真实驱动矩阵验证；未验证的 rate/SRC 拓扑继续 `host_limited`，不能只凭指针相同沿用旧配置。

## 实际 driver 通道表

GP UI 返回的输出选项 `[0]` 可以表示立体声对的起始 selector，不能解释为设备只有一个输出。开流构造 `PaAsioStreamInfo` 时，`0xA200..0xA21A` 把所选输出起点 N 和 N+1 写入 selector 数组；输入选择也在 `0xA11D..0xA142` 写 N、N+1，但实际输入 channelCount 由 `0xA0B4..0xA0C0` 决定为 1 或 2。

应读取成功开流后真正交给 driver 的表，而不是只依赖 UI 或 selector 暂存全局：

| 字段 | 本机偏移 | 语义 |
|---|---|---|
| inputChannelCount | `int32(stream+0x198)` | 已打开的输入通道数 |
| outputChannelCount | `int32(stream+0x19C)` | 已打开的输出通道数 |
| ASIOBufferInfo[] | `*(stream+0x180)` | `inputCount+outputCount` 项，输入在前、输出在后 |
| ASIOChannelInfo[] | `*(stream+0x188)` | 与 buffer 表同顺序的 driver channel metadata |

`ASIOBufferInfo` 每项 24 字节：`int32 isInput` 位于 `+0`，`int32 channelNum` 位于 `+4`，两个 driver buffer 指针位于 `+8/+16`。分配和填表见 `0x6EB41..0x6EC84`；`0x6ECD9` 直接将此表交给 `ASIOCreateBuffers`。输入 selector 写入 `0x6EB93/0x6EB96`，输出 selector 写入 `0x6EC24/0x6EC28`。未指定 selector 时分别从 0 起顺序编号。

`ASIOChannelInfo` 每项 52 字节：`int32 channel/isInput/isActive/channelGroup/type` 位于 `+0/+4/+8/+12/+16`，`name[32]` 位于 `+20`。`0x6EEE2..0x6EF43` 从 buffer 表复制 channel/isInput，逐项调用 `getChannelInfo` thunk `0x6A440`（driver vtable `+0x90`）。名称必须有界解码，不能假定带终止符。

因此逻辑右声道对应 `bufferInfos[inputCount+1].channelNum`，其名称来自同一索引的 channel info；这只确定 host 输出逻辑通道到 driver channel 的映射。用户物理右输出是否对应该 driver channel，仍须输入 2 的独立采样与可区分测试信号来确认，不能凭通常编号直接宣称是 1/2。

之前运行记录的 inputCount=1 只包含一个被选中的输入，不能读取不存在的第二个 buffer 项冒充输入 2。需要先记录实际表，确认 input 2 是否在打开集合；未打开时只能通过正常设备选择/重新开流取得它，并按新的 generation 验证。只读 inspector 可限制最多 8 输入与 8 输出，检查方向、数量、selector 和两张表一致，记录前后身份；即便身份相同，跨进程快照仍须标注 `concurrent_lifetime_race_possible`，不能成为在进程内长期缓存指针的依据。

## 当前 generation 与退役实现

`asio_lifecycle_probe.cpp` 的名称保留自调查阶段，其生命周期保护已进入普通构建；有界计时与导出仍只在实验宏下。一个进程只有一个 ASIO stream，状态使用 64 个不可复用代理和固定容量描述符，callback 不分配。

1. 每次 CreateBuffers 选择新的固定代理，generation 不复用；初始已开启流不会事后覆盖 callback 表，须经 GP 自身重建才能绑定。CreateBuffers 期间只开放必要通知，Start 开始前才允许音频 callback。每次 Start 推进 rate revision；同一 proxy 的 Start 不伪装成新的 CreateBuffers generation。
2. 外层 callback 经顺序一致 admission/reader 复查后才转原函数，退出时释放 reader；stream hook 内 input slot 再使用独立 `MonitorExchange` lease。被封闭的旧代理拒绝新准入，即使宿主分配器复用 native 地址也不变成新代。
3. Stop/Abort 入口先使 rate/started 资格失效，保留原 callback 完成原生 drain。原 Stop 返回值和观察到的 readers 只作为记录，不充当可以释放宿主对象的证明。
4. Close/Dispose 在调用原函数之前封闭外层准入并等待 readers 归零；2 秒后设置 `DrainOverdue` 并继续等待，不能返回“超时”后让 GP 释放仍被借用的 userData。永久不返回的第三方 callback 需要重启进程。等待只在控制线程，callback 重入控制入口在获取锁前拒绝，避免形成同锁等待环。
5. Start/Stop/Abort/Close、Create/Dispose 和 rate 查询共享非实时控制串行；Start 中通知会失效预先取到的 revision，不能由 Start 返回后的一次查询抹掉通知。原 Close 返回后不再解引用旧 stream。
6. input slot、native suppression、legacy fallback 与 stream watch 使用一次带代际的发布；激活后故障在已验证流上静音输入贡献。新流不继承旧流抑制，必须重新验证。production6 的 `watchStream` 修复使 Standard 或其他暂不支持状态保留用户请求，回到受支持 ASIO 流时自动重新准备；原生 Off callback 只能在其发布仍为 current 时清 suppression。

关闭 admission 的 generation 不得被当成新 generation 复用；否则一个在增加 reader 前被抢占的旧 callback 可能误认新描述符。reader=0 只是“当前没有已准入读者”，必须与先关闭入口、复查 generation 的协议组合。插件对象自身的退役合同也不能宣称已修复宿主内部 UAF 或第三方 driver 的异常 callback。

ASIO callback 参数没有 stream identity。若旧驱动通知在 hook 入口之前延迟，到新一代重新开放之后才第一次进入同一个函数，全局 generation 无法辨别其来源。必须证明 stop/dispose 后不会再交付旧通知，或使用按 generation 区分且退休后不复用的 callback 代理；单纯增加 sequence/generation 计数不能解决这类跨代通知。四个控制 hook 的串行机制也要验证 driver 回调重入控制路径，不能持锁等待一个必须取得同锁才可完成的宿主操作。

## 已有验证及剩余边界

静态调查证明四个生命周期函数与通道表的 ABI、Start 可能在返回前回调、Stop/Abort 的原生有界等待、Close 的先释放后清指针，以及 sample-rate wrapper 丢失错误码。当前实现已针对这些边界增加上述保护，不再只有观察 hook。

普通 production5 的 `artifacts/p13-host-8ad37b4898754ee59b33f953f3ef11b1/` 完成 5 轮 Off→On 与固定配置监听；production6 的 `artifacts/p13-host-bb8956b7cf2940e48a2d43b37358c33d/` 完成 Standard `host_limited`→ASIO 64 帧自动 active 和配置恢复，均正常退出。后者请求 128～4096 帧均被宿主拒绝并恢复 64，记录的是拒绝/恢复，不是多 buffer 运行通过。切换期间有 66 个累计输入错误块，不能将这一恢复测试标成全程零错误。

实验 probe33 的 `artifacts/p13-host-f0816daaa4d2447a8d80a1346ea330e9/` 在约 107 秒有效窗口观测 320936 次 ASIO proxy 与 stream hook，admitted=completed、快照一致、overlap=0、实际 192000 Hz/64 帧预算超时=0，ASIO overload/resync 通知为 0。外层 max 280 µs、P95 严格小于 61 µs，范围为 proxy 入口至原 driver callback 返回，不包含最终 reader 释放和 driver 调度；通知为 0 不代表驱动保证报告所有 xrun。前轮 `p13-host-1e9dac5fc0de493fab00572a83c22555` 的非一致快照及 1 次 hook 预算超时保留为未通过完整计时验收。最新停录 quiescence 修复仍待新构建验证。

离线专项覆盖未 Start Close、Open 失败 Dispose、Start 期间通知失效、回调重入控制拒绝、代际耗尽、安装失败回退及严格 MinHook 的 44 个故障场景。保护页/线程恢复严重失败保留实际补丁状态和重启要求，不能假成功。

尚缺真实 rate-change/reset/buffer 通知与实际变更的对应证据、Start 失败和 Stop/Abort 超时、异常 driver 回调、快速重建及完整三 scope 联合切换。其他设备、buffer 和采样率未验证；不支持的拓扑明确拒绝。P13 全计划还需长尾、录音/UI 电平、实例隔离组合与最终包回归，不能用本页生命周期证据代替。
