# P13 实现与验证记录

状态：P13-0～P13-4 的实现及固定配置验收已完成，版本 0.10.0（2026-09-19）。范围是精确匹配的 GP 8.1.1.17 和下述已验证 ASIO/SRC 合同，不代表所有设备、采样率或第三方插件已实测。

本记录区分工具实现、真实观测与尚未证明的合同。当前普通构建可在匹配的 Guitar Pro 8.1.1.17、实际 ASIO 192000 Hz、GP 内部 44100 Hz 和已验证 SRC 拓扑上启用独立监听；其他格式或合同不满足时为 `host_limited`。PCM、实验路由、逐样本观测和有界计时仅在 `GPVST3_P13_PROBE_BUILD` 中编译。早期普通构建的 `input_host_contract_not_released` 门禁已经移除，不应再作为当前行为；下方历史章节保留各轮采集当时的限制，不能将单项诊断 PASS 解释为 P13 完成。

## 独立监听当前实现与新增验证

- input 使用自己的 settings、effects、runtime pool、双槽、processor、参数/state 和 editor 归属；不会从 global/track 镜像。gain 更新及同采样率重绑保留实例与实时参数。顶层 `input` 保存 `monitor_mode`、`input_gain` 和 `effects`，不跟随切谱/切轨。
- `MonitorExchange` 用一个带代际的 token 同时发布 slot、native suppression 和 legacy fallback；callback 使用有界 reader admission。发布失败可在控制线程有界重试，slot 退役等 reader 退出后才改写。状态反馈同样携带 publication version，旧回调不能覆盖新 Off 状态。
- listener 原生 input、DSP、电平和返回值照常运行，其 output 分流到独占 sink；联合 SRC/ring 排空后，再把实际 ASIO capture 按原 callback 帧数送入独立 input VST3 并加到 GP output。故障丢弃整块输入贡献，不做 dry fallback 或混音后限幅。
- 只有 callback 已实际抑制 native listener 后，清链/准备失败才进入保持抑制的 mute；首次失败保留原路由。新 stream 不能继承旧流的抑制状态，必须重新核对身份和拓扑。准备失败及延迟到 Qt 的配置拒绝都检查最新请求代际。
- callback 使用 Win32 event 通知 worker，再由既有 Qt 通知更新 UI；没有增加周期轮询。Input UI 专项已纳入 `test-p13-input-ui.ps1`，100%/125%/150% DPI 通过。状态、重排失败回退、editor 退役、S0→S1→S2 参数保存及侧栏重建均有回归。
- P13 input state、50,000 次并发 exchange 发布、真实 VST3 配合合成流身份的 input runtime 专项均已通过。兼容写入修复了 schema-2 文档含旧 `plugin_path` 别名时误触发迁移并删除音轨配置的问题，P5/P8 state 专项也通过。
- 真实宿主 `artifacts/p13-host-b972bb7c9e7649c29a79274a3a3493be/` 使用 probe28，三次 Off→On 后 UI 均通过正常通知显示正确状态；192000 Hz、driver/callback/process 均 64 帧，处理 49251 块，错误及削波均 0，exit 0、无强退。512 块有界观测的 callback P50/P95/max 为 40.4/58.7/154.2 µs，原函数为 37.4/52.1/141.5 µs；这只是该短窗口，不能替代长期 deadline 或监听延迟。
- 真实 P8 runtime 回归 `artifacts/p8-runtime-95e078a7299441dd9c4cfcb3a07f2c3a/` 通过，正常退出；该证据验证原有异步链与 editor 行为，不替代 P13 混合音频验收。
- 生命周期 Close/Dispose 在原生释放前封闭准入并排空读者；超过 2 秒标记 `DrainOverdue` 后继续等，不能假装保留流而返回，因为 GP 会忽略错误并释放宿主对象。永久卡住的第三方 callback 需要重启。CreateBuffers 只允许通知，Start 才开放音频准入；callback 重入控制接口在加锁前拒绝。
- 原先的 `writeJump` 已删除。普通构建链接固定版本 MinHook 与生命周期模块，指令 hook 的启用和移除均使用严格暂停/检查线程的本地扩展；IP 落在补丁区则拒绝并有界重试，冻结后再次检查线程集合，不移动 IP。`MH_IsHookEnabled` 核对实际补丁状态，保护恢复或线程恢复严重失败保留 `restartRequired`，不把已经修改的指令误报为未安装。44 个 enable/disable 故障场景和四线程真实 trampoline 专项通过。64 个不可复用 proxy 用尽时，原生继续透传，独立监听报告 `input_stream_capacity_exhausted_restart_required`，不循环重置。
- 普通构建冷启动的 input SRC 会合法调用 listener `frames=0`；现已原样调用一次并把 output 指向 sink，不推进排空计数。负帧数和容量越界仍拒绝。专项和 production5 实机覆盖了此前导致永久 mute 的零帧问题。
- 当前生产代码修复旧 Off callback 覆盖新配置的 suppression 竞态，并以 `watchStream` 保留低延迟请求在 `host_limited` 后的流身份观察；回到受支持流时由 worker 重新准备。插件准备失败不会触发同流自动重试。格式证据绑定当前 publication token，未验证配置显示未知，并将配置拒绝和 DSP 错误分开计数。UI 显示实际旁通、通道、驱动/回调帧数、插件延迟和削波提示。实验观测不自行改变 Off 路由，停录先封闭 writer 再导出一致快照；最新普通及 probe39 构建均已复验。

## 0.10.0 最终音频与控制验收

以下记录均使用同一已验证宿主、Studio 2 PRO / Midiplus USB Audio、实际 192000 Hz / 64 帧。测试宿主正常退出（exit 0、无强退），原始曲谱、设备和监听设置恢复；`collected_unvalidated` 仅表示采集器没有自动替代人工逐项验收。

| 门禁 | 最终证据 |
|---|---|
| 首次切换与旧队列 | `p13-host-430f5c48c89a469dba7c36578c59f1e7` 从第一个实际抑制 callback 开始观测：398 块 preparing 后进入 active，生产 drain state/phase 与独立 retrospective tracker 逐块一致。排空期间输出逐位不变，Ready 后才叠加；4096 块全部有效，524288 个输出样本及 120422 个 RSE/SRC 样本零差异。 |
| 原生效果与尾音 | `p13-host-c348d9a1070f4d60b95a9a614e9fa656` 先启用并预热原生效果链，再经正常 UI 将 native pre-gain 置零。4096 次 listener 调用读回前置增益均为 0，sink 仍有 17.478061 的残留能量（排空期间 14.115100）；所有残留被隔离。399 块 preparing、3697 块 active，生产/独立排空计数一致；524288 个输出与 120422 个 RSE/SRC 样本零差异、无非有限或 caller 改写。 |
| 效果实例归属 | 同轮只读 RTTI 确認 listener 的独立 EQ、`M07_DynamicClassicDynamic`、`M04_StudioReverbRoomAmbience`，listener chain 无 Master 指针。`+0xD0` 为独立 `I01_VolumeAndPan`；RSE Master 的 EQ、limiter、reverb、volume/pan 地址与 listener 对象分离。RSE 已经经过 Master 再进入 unit PCM，sink 不重置它们。 |
| 原生 UI 电平 | `p13-host-9083ebc10cc645799c2bb1a162a2c4d4` 普通构建、原生效果链开启；可见输入电平在原生与 overlay 两段各 12 次采样中均动态非零，输入通道 1 对应 left，right=0。后续 native-effects 矩阵保留相同验证入口，不把内部 peak 或有限值当作全部 meter 验收。 |
| 三链与快速取消 | 最终普通 DLL `.tools/native/p13-release` 的 `p13-host-98199906536946a385991bd984df0121` 在播放中运行 5 轮完整 Off/On 和 5 轮不等待 active 的 On/Off，其中捕获一次 draining→Off。每轮最终 Off、native suppression=false、处理错误 0；input/track/global 三个不同实例分别 gain .25/.5/.75，启停保持各自参数；切谱保持 input 实例；清链实际 muted 且 suppression=true，重新启用恢复参数。 |
| 设备拒绝与自动恢复 | 同轮 128/256/512/1024/2048/4096 请求均被宿主拒绝并恢复原配置；Standard 为 host_limited、格式字段为 0、native suppression=false；返回 ASIO64 自动 active。最终 input 71609 块、DSP 错误/削波均 0；57 个重配置拒绝块独立记录，不能说全程没有配置失效。 |
| 长窗口与停录一致性 | probe39 的 `p13-host-782cb4f28984491ba771759e480e702f` 开启原生效果链、三链并行并进行 5 轮播放中切换。有效窗口 115.5768 秒，两层计时 admitted=completed=346728，quiesced/coherent=true；无预算超时、未验证预算、重入、status flags、处理失败、返回错误，overload/resync 通知为 0。输入总处理 371303 块、错误/削波为 0。 |
| 原生监听用户修改 | 普通构建 `p13-host-05a782c9f048457c8b904a98b8be4753` 在 overlay active 时分别关闭/开启原生 Line-In，再执行 overlay Off/On；四种组合均保留用户此时的 native checked 值，Off 解除分流、On 恢复 active。DSP 错误/削波为 0，track/global 持续处理；原始 native 开关与设备配置恢复、exit 0。 |
| 旧输入路由与链顺序 | 普通发布 DLL 的 `mcp-p8-order-ecd201c58964451389628fabcc3036e8`（input_insert）和 `mcp-p8-order-e06ce7ec15f34e2bb709cf1ea0644e16`（bus_mix）通过真实音频 ABC→CAB 非交换顺序、实例复用、重新启用和进程重启。P4 输出符合原有 capture/generated/process 公式；两次运行的前后进程均正常退出。 |
| 普通 DLL 音轨生命周期 | `mcp-p8-track-ae61d41686344c54a65fca6069498f57` 使用发布 DLL，通过交换/复制/删除/撤销、Save As、重开、重启及参数恢复，exit 0；该专项没有同时启用 input，不冒充三链共存证据。 |
| input 冷启动及保存 Off | `p13-input-startup-c9f4940cc92a489e83a1aea7c7df8341` 普通 DLL 三次独立进程启动；前两次不操作启用开关即自动 active，monitor gain .35→.55、插件 .25→.75 保留。第二次显式保存 Off，第三次自动保持 Off、无 native suppression、monitor/fixture 处理块均为 0，参数仍为 .55/.75。三次 exit 0，设备及 native listener 状态不变。 |
| 插件动态延迟 | `p13-latency-runtime.log` 的真实 VST3 与合成流专项验证两实例 17+29=46；插件实际发送 kLatencyChanged 后，worker 主动向 observer 发布累计值 117→100→0，未重建实例或变更 selection generation。默认 fixture 仍报告 0；这是延迟报告与通知验证，不是新增物理测量。 |
| 安装与卸载 | `p6-package-061794f79cf0407c847c4dc87918c48c` 及 `p13-release-package-test.log` 验证 manifest/ZIP 内容、安装归属、重复安装、未归属文件拒绝/显式迁移备份、修改后拒绝卸载与正常卸载。普通 DLL 实验字段隔离及实验 DLL 拒绝打包均通过。 |
| 最终发布复验 | 最新源码重新构建的普通 DLL SHA256 为 `414F494248017C344B23923AD80ACC5CB13849886A9857FF8A9FD4B4CC76CF40`，与上述实机矩阵 DLL 相同。包内 DLL 的 `p13-input-startup-35e3e03cca1347038a6d5c3383c1f2ae` 再次通过三次启动、参数与 Off 恢复；`p6-package-7fdfc8fae0bd429a8d9fa0215dfef9cc` 再次通过安装/卸载安全，manifest 全部 30 个文件 hash 与包内容一致。 |

上述路径均位于本地 `artifacts/`，不进入源码提交或发布包。尾音实验使用普通生产路由，额外观察代码只读样本；原生 gain 的变更通过正常 UI 完成并恢复，未用注入静音替代尾音证据。它验证已选混响的实际残留，不声称所有第三方效果均有相同尾音。

最新长窗口中 stream hook P50/P95 严格上界为 43/64 µs、max 306.9 µs；ASIO proxy 为 44/66 µs、max 310.9 µs；input router 为 2/3 µs、max 25.8 µs。64/192000 的预算约 333.333 µs。计时包含实验开销、排除 proxy reader 释放及 driver 调度；通知为 0 不等同于硬件绝无丢样。

离线最终回归见 `artifacts/p13-final-offline.json`、`p13-final-runtime.log`、`p13-final-ui.log`：旧/overlay router、排空及真实布局夹具、50,000 次 exchange 并发、input state/runtime/UI、ASIO 生命周期、严格 MinHook 故障注入、probe/PCM/timing 全部通过；Python 分析器 30 项通过。首次准备失败、过期准备取消、插件替换、非法样本/容量、别名、动态 rate revision 和异常/释放属于可重复故障注入验证，不伪称在用户硬件上制造过每种驱动故障。

快速取消初轮 `709436...` 读取了尚未更新的诊断文件；`a9e8...` 切谱后未等待新音轨开始处理，导致断言失败。最终 collector 对齐异步落盘与 transport/binding 完成后重新运行，完整通过；旧失败记录保留，不追溯改写。

旧 P8 顺序脚本的启动断言也已对齐 P12：保留 sidecar 的 enabled/order/state 意图，global 随工程恢复，track 必须显式激活；不再要求启动清空保存配置，也不从仅含 catalog 的 status.json 判断运行时。新进程单独保留 observation，验证恢复后的实际 CAB 处理。此前过时断言和测试曲谱保存提示造成的失败记录保留。

## 本轮真实验证与有效范围

| 项目 | 证据与结论 |
|---|---|
| 普通构建监听 | `artifacts/p13-host-8ad37b4898754ee59b33f953f3ef11b1/` 使用 production5，无实验环境入口，5 轮 Off→On、RSE 与 track/global VST3 处理通过；192000 Hz，driver/callback/process 均 64 帧，输入处理 49323 块、错误/削波/fault 为 0。collector 为 `collected_unvalidated`，正常退出、清理完成。开关循环发生在播放前，不计为播放中快速切换验收。 |
| 原有 P8 生命周期回归 | `artifacts/mcp-p8-track-6e180f88beff43e584934246d3fdf2c5/verification.json` 覆盖独立 gain、native effect 操作、同轨数交换、增删、Save As、重开和重启，宿主正常退出。同轨数交换曾漏刷新，当前按存活 native Track 的 index 变化刷新绑定；测试也核对 generation 与完整 track key。重开后的测试遵循音轨 runtime 默认关闭合同，显式启用并循环播放后检验恢复；global 按保存的配置恢复。此专项没有同时启用 P13 input。 |
| overlay 逐样本共存 | `artifacts/p13-host-501b8cd9c26d495484843440276458e4/` 使用 probe34、unity input fixture、input gain 0.5。4096 个连续 callback、同一 token/generation、524288 个混合样本均等于该块原 GP output 加独立 capture 贡献；120422 个 output SRC 输入样本均逐位等于 RSE unit 输出按原顺序求和。差异、非有限、caller 改写、overlap 和削波均为 0；RSE、输入及 native sink 能量均非零，track/global VST3 实际处理且无错误。 |
| 共存排空后的子窗口 | 上述观测独立计数的 Ready 子窗口为 3697 块、108692 个 RSE 样本零差异，RSE unit 能量约 3185.836、输入贡献能量约 8.615。它验证已排空稳态共存，不把观测器第 399 块 Ready 当作生产模式刚刚激活，也不证明任意跨会话渲染完全相同。parent gain 限定为 1 且未静音。 |
| 内部 peak 观察 | 同轮有 4096 次有限内部 peak 读取；input peak 全程相同，output peak 有变化。只证明正式 sink 路径中原 listener 继续执行且内部字段可读，不能据此宣称正常 UI 电平、录音或所有 meter 行为不变。 |
| 普通构建设备切换 | `artifacts/p13-host-bb8956b7cf2940e48a2d43b37358c33d/` 使用 production6。请求 128/256/512/1024/2048/4096 帧均被当前宿主拒绝并恢复实际 64 帧，不能记为这些 buffer 已运行。Standard 下 `host_limited` 且 native suppression=false；回到 ASIO 64 后无需重按开关自动 active。退出前配置还原，exit 0、无强退、清理完成。切换期间累计 66 个输入错误块，故此轮支持拒绝/恢复合同，不支持全程无错误声明。 |

### 监听延迟对照

`artifacts/p13-monitor-delay-comparison.json` 比较同一声卡、192000 Hz、64 帧配置：native 为 `p13-host-14251005e9144e4f8e1e89dc27fd3869`，overlay 为 `p13-host-d02b297a5db24cad8b18315a7b9f75e7`。输入 1 是同步参考，Analog Out 2 经物理线接 Analog In 2；实验构建将 input 1 复制到左右监听，input 2 只录制，避免反馈。

| 模式 | 候选延迟 | 相关系数 |
|---|---:|---:|
| 原生监听 | 15064 samples，78.458333 ms | 0.830410 |
| 独立 overlay | 610 samples，3.177083 ms | 0.881608 |

三个时间分段的 lag 稳定，估计减少 75.28125 ms，未降低相关性或歧义拒绝门槛。口径包含软件监听路径、DAC、物理线和 ADC，不是 CPU 耗时或 driver 报告延迟。该实验使用无 RSE 音符的空白 Steel Guitar 曲谱；overlay gain 为 2，整场记录 122 个削波块，故只支持延迟结论，不支持未削波保真或 RSE 共存结论。两轮正常退出并恢复配置；analyzer 继续输出 `p13_acceptance=false`，其 15 项拒绝/一致性专项已通过。

### 有界长窗口计时

`artifacts/p13-host-f0816daaa4d2447a8d80a1346ea330e9/` 使用 probe33，同时观测 input router、stream hook 和外层 ASIO callback proxy。有效窗口约 106.979 秒，并非完整 120 秒；320936 次 admitted 与 completed 相等，两个快照均 coherent，overlap、status flags、返回错误、输入处理失败、身份变化和预算超时均为 0。实际 rate=192000、frames=64，对应每块预算约 333.333 µs。

| 测量边界 | P50 严格上界 | P95 严格上界 | 精确 max |
|---|---:|---:|---:|
| ASIO proxy 入口至原 driver callback 返回 | 42 µs | 61 µs | 280 µs |
| stream hook 入口至出口 | 40 µs | 59 µs | 278.4 µs |
| input router 处理 | 2 µs | 3 µs | 39.9 µs |

P50/P95 使用 1 µs 桶的严格上界，计时包含实验开销。外层范围包括 GP callback 后的原生输出转换等处理，但不包含最终 proxy reader 释放和 driver 调度。观测 ASIO overload/resync 通知均为 0；驱动可能不报告所有 xrun，所以不能将通知为 0 扩大为物理设备绝无丢样。

前轮 `artifacts/p13-host-1e9dac5fc0de493fab00572a83c22555/` 的 hook 快照为 incoherent，admitted=319714、completed=319713，并有 1 次预算超时（max 335.7 µs）；缺少外层 driver 计时，不能作为完整长窗口验收。该记录保留原状。停录/quiescence 改进已由上述 probe39 新采集验证，不将修复追溯到旧采集。

## 发布范围与设备限制

- 生产范围按宿主 hash、ASIO stream generation/rate 与 SRC 拓扑门控。其他采样率保持 `host_limited`；不同声卡、真实 rate 改变及异常驱动通知尚未硬件实测，不能扩大为通用 ASIO 兼容声明。当前硬件拒绝的 buffer 列为宿主受限，可变帧算法另有离线容量/处理长度验证。
- 原生音频录音为宿主不提供／不适用。GP8 官方手册 PDF 第 315 页明确 “There is not any record feature in Guitar Pro 8”；来源与 UI/字段调查见 [宿主边界](P13_HOST_BOUNDARY.md)。输入电平已实测，实验 PCM 不算原生录音。
- 任意第三方插件的崩溃、死锁及 CPU 过载仍属于进程内风险；本阶段提供整块错误静音、削波/插件延迟诊断及安全退役，不承诺进程隔离或零延迟。
- 0.10.0 使用普通构建打包；PCM、逐样本 observer、实验路由和长计时入口不会进入发布 DLL。包内 manifest 记录各文件 SHA256，release 附带 ZIP 校验文件；安装/卸载与普通 DLL 生命周期证据见上表。

## 历史调查记录

以下章节保留早期调查的原始证据范围。文中的“尚未接入”“未完成延迟”描述对应当时构建，当前状态以上述实现和本轮证据为准；失败记录和历史不足不改写为成功。

### 早期 P13-0 工具

- `native/build.ps1 -EnableP13Probe` 生成独立实验 DLL；普通构建不包含 probe、静音输入实验和实验环境变量入口。`native/package.ps1` 拒绝包含实验 marker 的 DLL，拒绝发生在创建 staging 前。
- `input_probe.h` 提供固定 512 槽、一次配置、不可复用的 recorder。每块最多保存输入及原 callback 输出各 16 帧，采用 release/acquire 发布，未发布记录不可读；callback 中无分配、Qt、磁盘操作或等待锁。多写者各自领取唯一槽，控制线程逐槽读取已发布记录。
- 仅 hash/prologue 匹配的 `streamCallbackHook` 接入诊断。原 callback 前采集 capture；原 callback 成功返回后、legacy router 前复制已验证范围内的输出。未初始化的原 callback 前 output 不作为音频证据；probe 运行时关闭旧的 output 前后 hash 观测。
- 控制线程在有上限的实验窗口导出 `p13-input-probe.json`；排序、分位数与 JSON 写盘均不在 callback 内。无采样时不得推断音频状态；时间统计包含部分 probe 开销且不包含最终 recorder 发布，不代表完整设备 deadline。
- `input_pcm_probe.h` 新增一次性连续 PCM recorder，在控制线程预分配最多 262144 帧和 8192 条记录；callback 只复制到自有存储并发布，重入立即放弃本次采样。实验导出保存原始 float32 PCM、逐块字节偏移、序号、状态及 SHA-256，不把不同通道数的记录按固定 stride 解码。直接 ASIO buffer/channel 表中的 selector 也逐块核对并保存；新记录还逐块保存已验证的 generation、rate revision 和实际采样率；旧记录仅有事后控制线程查询值。
- `collect-p13-host.ps1` 校验宿主与实验 DLL，使用独立数据目录、曲谱副本和空插件扫描目录；仅操作自身进程。采集记录始终标为未验收。`analyze-p13-probe.ps1` 离线核对序列、短样本、帧数、设备状态、采样窗口与退出结果。
- 静音 input 候选仅存在于实验构建，需显式环境请求与副本曲谱标志，且仅在有效格式/容量和采样 ticket 内替换输入；不是正式旁通。窗口结束恢复原输入指针，不清理历史队列或尾音。本次尚未执行该候选实验。
- `-ProbeListener` 通过 EXE hash、vtable 和完整 prologue 验证后记录独立监听单元；安装/移除使用对齐虚表槽的原子 compare-exchange，原函数地址保留供已进入的调用使用，不再改写正在执行的 listener 指令。以外层 callback 序号、线程和时间窗口关联，保留真实返回值。`-EnableNativeListener` 通过已观察的 `actionActivatedLineIn` 开关启用，并在清理时恢复原 checked 状态再次确认；读取到模态警告时保留证据并失败，不猜测按钮。
- 实验构建新增有界 listener sink：只在显式请求、副本曲谱、已验证双通道和有效 ticket 下，将该单元的 output 参数改为独占的预分配缓冲。真实 input、原生 DSP、电平更新和返回值保留；重入不等待，记录 busy 后转原路径。它不排空共享 SRC/ring、不输出 overlay，也不是正式低延迟模式。

静态字段、函数/RVA 和模块 hash 见 [宿主边界调查](P13_HOST_BOUNDARY.md)。

### 历史离线与第二轮宿主验证

离线证据：`artifacts/p13-offline-verification.json`。

- 当时普通生产配置构建成功，DLL 不含实验 marker、probe 导出或实验配置字符串；当前生产接入后的最终二进制仍需重新完成发布隔离检查。
- `test-p13-probe.ps1` 通过 `/W4 /WX` 构建与专项：固定容量、并发领取/发布/读取、未发布不可见、guard page 禁读、非有限值 mask、别名复制和 callback 无分配。
- `test-p4-router.ps1` 通过 `/W4 /WX`，同时覆盖旧路由与新 overlay 内核。后者使用独立 scratch、完整有限值检查和整块提交；失败保持 GP output，不限幅，不将 GP output 送入输入 processor。覆盖单/双声道、同址/部分重叠、非法输出重叠、1/64/65/127/128/256/512/1024/2048 帧、越界和回调零分配。当时内核尚无生产入口，该轮单测不代表宿主旁通完成。
- `test-p13-drain.ps1` 通过 `/W4 /WX`，验证计数状态机的门限、队列连续性、配置变化和错误终态；当时仅接入下述联合排空实验，尚未进入普通构建。
- `test-p13-pcm-probe.ps1` 通过 `/W4 /WX`，覆盖完整 PCM、别名、guard page、容量与尾部截断、身份变化、并发发布/跳过和 callback 零分配。接入实验 DLL 构建通过；尚未据此认定物理回环延迟。
- 实验 DLL 被发布打包器拒绝且未创建 staging。

真实宿主第二轮证据：`artifacts/p13-host-062bf4d0eacb49f99db4ada6498894e7/collection.json`、`probe-analysis.json`、`host-integrity.json`、`shutdown.json`。

| 项目 | 实际观测与范围 |
|---|---|
| 宿主/设备 | Guitar Pro 8.1.1.17，所有目标模块 SHA-256 匹配；Midiplus USB Audio ASIO，前后配置未改变 |
| 实际 ASIO 身份 | 当前 callback stream 与 `theAsioStream` 相等，stream owner 和 ASIO interface 三个函数地址匹配 |
| driver / callback block | 512 条记录中，分别独立读取的 driver block 和 callback frames 均为 64 |
| 采样率 | `PaStreamInfo` 请求值为 44100；控制线程 `PaAsio_GetSampleRate` wrapper 返回 192000。后续静态审查确认 wrapper 丢失驱动错误码，该轮结果不能当作真实 `ASIOError=0` 的证据；两种 rate 不能混用 |
| 通道 | 输入 1 通道、输出 2 通道 |
| 输入/输出短样本 | 512 个输入片段和原 callback 输出片段均非零；无非有限 mask 或样本形状错误。不能据此证明分离后的 RSE 与 input 共存 |
| 时间窗口 | 约 170.127 ms；播放状态观测包围该窗口，不等于连续播放/可听声音证明 |
| 连续性 | 此窗口 sequence gap/duplicate 为 0，callback status flags 均为 0 |
| callback 计时 | P50 29.7 µs，P95 66.6 µs，max 130.2 µs；仅部分 callback 计时，包含诊断开销，未证明完整驱动 deadline 或长期稳定性 |
| 驱动报告延迟 | 输入 352、输出 288 samples；不是物理输入到输出延迟 |
| 清理 | 自身测试宿主正常退出，exit code 0、无强退；安装目录与原始曲谱不变 |

第一轮 `artifacts/p13-host-ac16e3acf90744e88efd51a0f684c88a/` 已取得输入片段，但尚无独立 driver block/rate，output 记为 unvalidated，缺少播放窗口时间戳，最终强制退出。其旧 `observed_deadline_misses=0` 采用请求采样率，不能用于实际设备 deadline；后续分析明确忽略这一字段。第二轮补齐了上述观测与正常退出，但没有补齐输入隔离合同。

### 历史原生监听开启证据

第四轮 `artifacts/p13-host-b5a97fa73931498988f0ecb13b85c842/` 的 512 次 listener 调用全部关闭，故只能证明 capture 到达内部单元，不能作为启用监听的验收。其内部双通道 block 为 14/15 帧，与外层 192 kHz、64 帧的转换一致。

后续正常 UI 会话 `artifacts/p13-listener-ui-6c1d6e3db6ff4ef5930b4af0ef8dc81b/` 打开副本曲谱后恢复窗口，经 fresh QAction 快照开启监听；没有模态警告。开关 false→true、内部 listener enabled=true 且 peak 非零；退出前恢复 false，宿主 exit code 0、无强退。

同时播放 RSE 的采集 `artifacts/p13-host-fe487f3eea0941f59c97c540458606a8/` 中，512 次 listener 全部开启，全部与父 callback 的线程/时间范围对应；单位 block 为 14 帧×153、15 帧×359。capture、listener 处理输出和设备输出短片段均非零有限；外层仍为 64 帧，无序号缺口。观测 callback P50/P95/max 为 38.8/56.2/158.1 μs，仅代表该短诊断窗口。原生监听恢复 false，宿主正常退出，但关闭 HTTP 会话发生传输异常，原始 collection 严格保留 `cleanup_failed`，不追改为成功。

这些证据补齐了“监听确实开启”的前提，尚未证明旧输入排空或 RSE 逐样本不变。共享 SRC 的有限依赖与候选排空规则见 [排空分析](P13_SRC_DRAIN_ANALYSIS.md)；该规则尚未完成真实切换验证。

随后 `artifacts/p13-host-21fb8b26f0274f92b6b5d35adba59e62/` 的 sink 实验取得 512 次启用状态 listener 调用，全部应用 sink、无 busy。原 caller output 保持全零，sink 包含非零有限样本，原生 DSP、peak 与返回值继续运行；外层 callback 64 帧，内部 14/15 帧。这证明该窗口的 listener 输出参数分流生效，仍未排空共享 SRC/ring，也不是最终 RSE/输入共存验收。清理无错误，宿主正常退出。

### 历史流与实际通道映射

生命周期调查见 [P13_STREAM_LIFECYCLE.md](P13_STREAM_LIFECYCLE.md)。该轮已定位 Start/Stop/Abort/Close、实际 driver channel 表及 rate-change 通知，但 generation 与退役保护当时尚未接入普通构建。实验 rate 查询改为直接调用 `0x6A540`，预置 NaN 并严格检查真实 `ASIOError` 和有限 rate；控制线程当前值仍不能冒充历史窗口的 generation 绑定。

用户已连接右声道输出到输入 2。独立宿主 `artifacts/p13-channel-map/channels.json` 的实际 ASIO 表显示：左右输出分别为 `Analog Out 1`（driver channel 0）和 `Analog Out 2`（1）。长按 `lineInButton` 打开原生设置，通过 `inputDeviceComboBox.currentIndex=1` 选择输入 2 后，`input2-channels.json` 确认实际 stream 已切到 `Analog In 2`（1），输入 count 仍为 1。选择期间先将原生监听增益置零；清理恢复输入 1、增益 500 和监听关闭，宿主正常退出。`gp_audio_device.choices.audioOutputChannels=[0]` 是整数属性的当前值回显，真实输出 UI 为 `1/2 (stereo)`，不能据此推导只有一个通道。上述证据确认软件通道身份和切换，物理回环的信号相关性与延迟仍需连续 PCM 验证。

### 历史连续 PCM 与物理回环

`collect-p13-host.ps1 -PcmProbe -LoopbackInput2` 通过正常 UI 选择回环输入、关闭原生监听，并在结束后恢复输入 1、增益和开关。每次采集 4096 个 64 帧 callback，共 262144 帧；输入文件 1 MiB、双通道输出文件 2 MiB。文件 SHA-256、逐块 offset、序号、状态、有限值与播放窗口均单独核对。新记录逐块验证 `ASIOBufferInfo` 与 `ASIOChannelInfo` 的 selector 一致，整个采样窗口为输入 `[1]`、输出 `[0,1]`。

最初 `a5d9b942...`、`eb9d4752...` 与首次调增益后的 `44ec28de...` 记录相关性不足，保留 `inconclusive`。用户再次提高硬件电平后取得以下条件估计；没有降低分析门槛来接受早期记录。

| artifact 目录后缀 | 候选 lag | 192 kHz 下的毫秒估计 | 相关系数 | 说明 |
|---|---:|---:|---:|---|
| `f2ae8daf4cb54a7793aaf18bbe59ba75` | 590 samples | 3.073 ms | 0.733 | 首个通过相关/歧义/分段一致性检查的窗口 |
| `cbaa5248680b4101a0fa7c73fb4902bb` | 633 samples | 3.297 ms | 0.834 | 无削波，仍为事后 rate 观测 |
| `26684249d37249fbb01036fe562b3214` | 629 samples | 3.276 ms | 0.762 | 全部块的 generation/revision/rate 通过验证，分析器使用逐块实际 rate |

这些值是 **callback output → DAC → 物理线 → ADC → callback capture** 的往返估计，不是原生或低延迟 input 监听延迟。RSE 吉他信号的相关峰较宽，分段候选也有偏差，不能报告单样本精度或把不同会话之差当作优化量。该阶段尚未完成改造前后监听对照，后续结果见本轮延迟对照章节。

`analyze-p13-pcm.py` 用 NumPy FFT 计算按重叠范围归一化的 Pearson 相关，拒绝低相关、歧义峰、不一致分段、错向延迟与损坏记录。15 项合成测试通过，包含与直接计算对照、已知延迟、反相、变长块、通道映射变化、逐块 rate 验证、事后查询差异及损坏拒绝。仅当全部块具有一致且有效的 generation/revision/rate 时输出 physical_loopback_estimate；旧记录或显式 rate 覆盖继续输出条件估计。

### 历史生命周期实验接入

该阶段新增 `asio_lifecycle_probe.cpp`，当时仅实验构建链接固定的 MinHook v1.3.4，普通 DLL 尚不包含该模块。`-StreamLifecycleProbe` 在完整模块 hash 和入口字节门控后观察 CreateBuffers、DisposeBuffers、Start、Stop、Abort、Close。每次 CreateBuffers 使用新的固定代理入口，最多 64 代，旧入口和 context 不复用；达到上限或 callback 表不匹配时保留原调用并报告未绑定。rate/reset 通知使当前 rate revision 立即失效；控制线程查询与这些生命周期调用串行。

`d7993cd08bb445ac9b431f98fb7a425d` 和 `26684249d37249fbb01036fe562b3214` 的 4096 个 PCM 块全部观测为 generation 1、revision 2、192000 Hz、rateValidated=true。后一会话恢复输入 1 时，generation 1 的 Stop/Close 均返回 0，观察到的活动代理 reader 均为 0；随后 generation 2 成功创建并 Start。Qt shutdown 快照早于最后一代的 native Close，不能宣称已经捕获该次最终关闭。

上述采集所用版本仅是观察工具，Close 当时未等待活动 callback，非零 reader 应判生命周期合同未通过；后续生产实现已增加封闭准入与排空。初始已打开的流没有事后替换 driver callback，记录为 unattached；只有后续 CreateBuffers 才可建立新代理代际。该轮没有 Start 失败、Stop 超时、真正 rate/reset 通知、64 代耗尽和压力切换的完整验收，不能单独作为生产接入依据。

### 历史联合排空与 RSE 逐样本证据

`-DrainProbe -StreamLifecycleProbe -RestartAsioStream -ProbeListener -EnableNativeListener -SinkListenerExperiment` 使用 GP 自身 Qt reset dispatcher 重建同配置流，为后续 CreateBuffers 绑定代际；不创建第二个声卡 stream。实验在连续 4096 个外层 callback 内保持监听分流，记录容量与实际抑制独立，512 条 listener 短记录用尽不会提前恢复监听。窗口结束才恢复原生输出贡献，整个实验不输出 overlay、不清共享 DSP/SRC/ring。

`input_drain_probe.h` 在已识别 callback 借用范围内验证 SRC 对象、左右声道拓扑、动态边界和 ring 身份；`0x51890` hook 仅计数当前 output SRC，忽略 input SRC。实际返回帧数与 ring 前后 count、read/write index 一起核对，read 模差独立提供消费量。计数状态机只按实际 SRC input frames 推进。非零 callback status、空输出、重入、配置变化、非有限样本或计数错误均不能通过。

最后一轮 `artifacts/p13-host-51f02261a98d4c0888903124b5cbb183/`（实验 DLL probe18）的 `data/p13-input-probe.json` SHA-256 为 `5A9CA765CF04744E68BF88AA0E1FE5F21039AB30B09692DBF22629F862C13231`：

| 项目 | 已观测结果 |
|---|---|
| 格式与窗口 | generation 1 / revision 2 / actual 192000 Hz；4096 × 64 帧，约 1365 ms；完整窗口被正常播放观测包围 |
| SRC 拓扑 | 两声道均为一层 2× convolver + 88200→192000 interpolator；InputLen=2680、PrevInputLen=708、BlockLen2=4096、Latency=3388、KernelLen=1417 |
| 排空门限 | convolver 4020 个源帧，interpolator 1822 个后续源帧；index 399 首次 Ready，约 133.012 ms；后续 3697 条均满足 Ready |
| 连续性 | 4096 条完整验证；overlap、status flags、nonfinite 和原 caller 改写样本数均为 0 |
| RSE 对照 | 12288 次 GPRSE unit 调用的实际输出按原顺序相加，与送入 output SRC 的 120422 个 float 样本逐位相等；非零 RSE 能量已观测 |
| 输入隔离 | 同一窗口 capture/native sink 能量非零；listener 真实 input、DSP、状态更新与返回仍执行；其 caller scratch 保持逐位不变 |
| 恢复 | 声卡配置前后相同，原生监听恢复为原先关闭，cleanup 无错误，宿主正常退出 |

RSE 对照通过原子虚表槽 hook 观察 GPRSE `fillBuffer`，不改其参数或返回值。AMAudio 在 unit 求和后还会乘 parent output gain；本次比较明确限制 parent gain=1 且 parent 未静音。此证据证明该窗口送入共享 SRC 的信号等于已渲染 RSE PCM 之和，不代表跨会话渲染状态、任意增益或所有效果组合都已验收。原监听尾音仍在其 DSP 内运行并被分流；约 133 ms 是一次保守准备过渡，不是稳定输入监听延迟。

生命周期专项新增安装失败逐项回退、回退失败纯转发、未 Start 即 Close、Open 失败直接 Dispose、Start 期间 rate/reset 通知失效，以及 SRC observer 的对象筛选/重入/TLS 隔离。测试通过 `/W4 /WX`；真实失败、超时和长时压力路径仍未覆盖。生产构建 probe-check4 成功，未包含 P13/MinHook 实验入口 marker。

### 历史 RSE 输出队列边界调查

真实进程只读枚举发现三个 GPRSE AudioUnit 使用 vtable RVA `0x23AD98`、`fillBuffer` RVA `0x492B0`，另有一个 EXE 监听候选 unit。匹配版本 GPRSE 的静态调用链证明：`0x492B0` 不保存或读取传入的 capture 指针/通道参数；它在 `0x4955F` 调用 `0x49DA0`，从自身 `+0x198` 的 PCM 环读取已渲染结果并写入该 unit 的 output。

RSE 生产端 `0x48570` 经 `0x487BD → 0x47890 → 0x47BA0` 调用 `Conductor::fillBuffer`（`0x3DFE0`）；随后 `0x3E204` 调用 `Master::process`（`0xBDC90`），`0x3E22F` 将 Master 后的结果加入生产 PCM。`Master::limiter()`（`0xBDC80`）返回 impl `+8`，对应的效果处理调用点 `0xBE224` 位于 PCM 发布之前。生产端在 `0x48A09/0x48A1A/0x48A2E` 将 PCM 复制到环数据区，`0x48A3B` 更新写位置；其读位置 `+0x1D8`、容量 `+0x1E0`、数据指针 `+0x1E8` 与 callback 消费端完全对应。

这些证据确认了 RSE 的生产和队列消费边界，支持保留其原有 Master 处理与调度，不能把 RSE 渲染改塞入输入 callback。它们尚不证明 EXE 监听链与 RSE 的效果实例、共享输出处理和生命周期完全隔离，也不能替代两路同时运行的声音验证。动态枚举未暂停进程，只证明观测时的函数身份，不证明对象回收或流重建合同。

证据：`artifacts/p13-host-boundary/units-53472-20260918222809765.json`、`GPRSE_INPUT_BOUNDARY_REVIEW.md` 及对应反汇编片段。模块 hash、完整宿主调用链和剩余边界见 [宿主边界调查](P13_HOST_BOUNDARY.md)。

## 当前支持边界

真实设备目前按 192 kHz 提供 capture，而 GP RSE 仍使用 44.1 kHz。既有 `portaudio_capture_abi.h` 从 `PaStreamInfo` 读取的值不能直接用作低延迟输入 processor 的准备采样率。P13 必须在控制线程获取并验证实际 rate、绑定 stream generation，并在流重建时重新准备；不能只将 64 帧传给按 44100 准备的 VST3。

联合实验及本轮实际 overlay 观测已经在固定配置证明原生监听输出分流、SRC/ring 保守排空、SRC 输入等于 RSE unit PCM 之和与独立输入加法合成；前后监听延迟对照也已取得。剩余门禁见上，不能将固定配置证据扩大为所有设备、效果组合和用户操作均通过。

普通构建已完成本计划在固定宿主与设备合同下的实现及验收。其他硬件和格式按计划保留为未验证/宿主受限；完整证据包括真实音频、生命周期、故障夹具及安装包，不能只用单项工具 PASS 代替。

## 复现入口

```powershell
./native/test/test-p13-probe.ps1
./native/test/test-p13-drain.ps1
./native/test/test-p13-drain-probe.ps1
./native/test/test-p13-asio-lifecycle.ps1
./native/test/test-p13-pcm-probe.ps1
./native/test/test-p4-router.ps1
./native/build.ps1 -EnableP13Probe -OutputRoot .tools/native/p13-probe
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll -PcmProbe -LoopbackInput2 -StreamLifecycleProbe
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll -DrainProbe -StreamLifecycleProbe -RestartAsioStream -ProbeListener -EnableNativeListener -SinkListenerExperiment -ProbeDelayMilliseconds 15000
./native/test/collect-p13-host.ps1 -PluginPath .tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll -InputOverlayFixture '.tools/native/p12-gain-fixture/P7 Gain Fixture.vst3' -OverlayCoexistenceProbe -StreamLifecycleProbe -RestartAsioStream -EnableNativeListener -RseVst3 -ProbeDelayMilliseconds 20000
./native/test/collect-p13-host.ps1 -ProductionRuntime -PluginPath .tools/native/p13-production/plugins/imageformats/guitarpro_vst3_autoload.dll -InputOverlayFixture '.tools/native/p12-gain-fixture/P7 Gain Fixture.vst3' -InputOverlaySwitches 5 -EnableNativeListener -RseVst3
./native/test/analyze-p13-probe.ps1 -CollectionPath artifacts/<本次目录>/collection.json
```

默认采集不改设备配置、不执行静音输入。真实静音候选实验需另行明确实验窗口和观测目标。源码、配置脚本及说明可以提交；`.tools/`、`artifacts/`、实验 DLL、模块转储及客户机配置不能提交。
