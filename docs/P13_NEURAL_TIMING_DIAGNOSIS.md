# Neural DSP 192000 Hz / 64 帧计时调查

本记录汇总 0.10.1 后续自动验证并随 0.10.2 发布，日期为 2026-09-20。参数 mailbox 的无效实时开销和输入输出范围已修复。用户随后实测确认：开启声卡驱动安全模式后，原先 Neural DSP 吉他演奏的爆音消失，并要求保留完整优化代码进入发布。该结论适用于用户当前配置；下方探针超时记录仍保留，不改写为所有配置零 xrun。

## 测量范围

设备为 Studio 2 PRO / Midiplus USB Audio，CPU 为 AMD Ryzen 9 5950X；匹配 Guitar Pro 8.1.1.17，插件为 `Archetype Mateus Asato.vst3`。下面有效的宿主会话均只在 input 链启用一个 Neural，保留 RSE 播放，实际格式为 192000 Hz / 64 帧；没有同时加载三份 Neural。

实验 DLL 在 `GPVST3_P13_PROBE_BUILD` 下保留最多 256 个超时 callback 的同块 native/input 时间边界、线程/CPU 和流身份；另保存最早 256 个实际处理输入的 callback，观察正常块的连续节奏。固定数组写完后 release 发布，数据不复用；控制侧 acquire 读取，音频侧不分配、不等待读者。记录有序号，但发生关闭输入、重入或格式失效时不保证序号连续，分析必须核对。

`QueryThreadCycleTime` 仅在显式 `-TimingCycles` 时启用，默认只用 QPC。cycle 不是纳秒，也不能据此排除 cache stall、抢占和中断。`-PinCallbackExperiment` 和 `-IdealCallbackExperiment` 互斥，只允许明确创建的实验进程，在一个 processor group 中分别设置当前 callback 线程的亲和度或首选核心，随测试进程退出而结束；普通 DLL 不包含这些入口。没有修改系统电源策略、全局亲和度或用户宿主进程。

## 宿主观测

每轮请求持续采集 45 秒；计时窗口与采集控制窗口不同，可能含清理阶段。各会话均正常退出、恢复原生设备与 LINE-IN，未报告输入 DSP 错误。这里的超时是测量范围超过预算，不等同于已测得物理丢样数。

| artifact 后缀 | 调度实验 | cycle 查询 | 完整 hook 块数 | hook 超时 | input 单独超时 | input P95 上界 |
|---|---|---|---:|---:|---:|---:|
| `89d452d27af64c3e8058930b34dc66e8` | 无 | 开 | 135521 | 1309 | 172 | 209 µs |
| `58f22bfe5a15427e95532fada6b5f7da` | 固定 CPU 30 | 开 | 132830 | 62 | 3 | 173 µs |
| `8d99bce5465d40ff82055c0373d2da71` | 首选核心 | 开 | 118623 | 1195 | 122 | 209 µs |
| `2b0bf64dffe044cda254e6524d349f05` | 无 | 关 | 118776 | 706 | 73 | 208 µs |

证据位于 `artifacts/p13-host-<后缀>/data/p13-runtime-final.json` 及同目录的 `collection.json`。首选核心的首次会话 `70a0497e547749309f6a98e7cc6d147a` 未能开启原生 LINE-IN，按失败保留，不计入性能结果。

固定核心的一轮改善明显，但还有超时；首选核心没有同样改善。无固定核心与首选核心的前 256 条超时分别有 61、49 条在 callback 首尾位于不同 CPU。最早超时样本的覆盖时长不同，不能把它们当作各运行的均匀分布样本，也不能单凭一次结果把永久 affinity 带入生产。

关闭 cycle 查询后仍有超时。该轮正常连续 256 块无序号缺口：input 偶数/奇数块中位分别为 160.75/6.0 µs，native 分别为 37.3/39.1 µs。两组 native 耗时近似，重 input 独立于 native 轻重；这与下方离线插件分段证据一致。该轮最大 hook 时间约 1.988 ms，其中同块 native 约 1.781 ms，也证明不能把所有峰值归于输入包装层。该会话采集中 LINE-IN 被宿主关闭，后半段为 `waiting_for_input`，只能分析其中有效处理的块，不能作为完整持续监听性能证据；后续固定核心会话 `f211b7319c8042f581647fa4ccbca7be` 同样因 LINE-IN 中途关闭未计入性能结论。

## 单独插件分段验证

`test-p13-neural-runtime.ps1` 使用合成拨弦、生产 `RuntimeEffect → audio_adapter → input_router`，没有声卡、GP callback 或 SRC。测试专用透明 `IAudioProcessor` delegate 只对真实插件的单次 `process` 计时；原生产代码和传入 `ProcessData` 不变。每组预热 1 秒，处理 24000 块，另保存最早 256 块，核对每块恰好一次 64 帧调用。

证据为 `.tools/native/p13-neural-stage-probe/timing-192000-64.json`，日志为 `artifacts/p13-neural-stage-probe.log`。生产对应的 `setup_max_frames=64`、FTZ 关闭组：

- 插件 `process` 偶数块 P50 为 3.0 µs，奇数块 P50 为 122.5 µs，首 256 块严格轻重交替。
- router/adapter 除插件外的残余时间 P50/P95 为 0.7/0.8 µs；总路径最大 247.0 µs，0/24000 块超预算。
- 四组（64/2048 准备容量 × FTZ 开/关）输出 float 位模式的 FNV1a64 均为 `dc836c10a3d2ac13`，插件报告延迟均为 159 samples，非有限和超满幅样本均为 0。hash 用于本次样本一致性检查，不是密码学签名。

因此每两块一次的重处理来自插件内部，GP SRC 或宿主重复调用不是该交替的必要成因。此测试不按真实时间节奏运行，不能用它的零超时替代 ASIO 调度或物理输出验收。输入 P50 恰好位于双峰交界，不用 P50 的跳变计算整体性能提升倍率。

## 当前结论和剩余条件

保留原生 listener DSP、meter、效果历史和尾音演进，不跳过原生调用来换取表面计时收益。不改插件 bus、采样率、buffer、预设、延迟或音频块顺序；不将实验固定核心作为已验证的通用修复。

自动验证已定位并修正宿主参数同步与输出保护的实际问题，也排除了重复调用及 SRC 引起输入奇偶重块的假设。用户已确认声卡驱动安全模式解决其当前演奏场景的爆音，本轮按该反馈完成问题收尾与发布。软件保留全部优化，不自动修改驱动安全模式；需要在声卡原生控制面板中设置。安全模式下的实际物理延迟、完整硬件矩阵及连续输出丢样计数未重新测量，不能沿用历史延迟数值或将听感反馈扩展为所有设备零 xrun。
