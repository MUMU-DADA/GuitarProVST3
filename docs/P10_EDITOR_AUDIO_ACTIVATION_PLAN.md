# P10：VST3 原生 UI 与首次启停音频生效优化计划

状态：已实施并完成夹具、Qt/HWND 与 MCP 宿主回归（2026-09-13）
关联记录：[P9 实现记录](P9_IMPLEMENTATION.md)、[实时实现总览](REALTIME_IMPLEMENTATION_PLAN.md)
基线：Guitar Pro 8.1.1.17 / Windows x64 / 当前 `v0.9.6` 工作树。

本计划根据真实使用反馈重新制定，处理两个相互关联但验收标准不同的问题：

1. 已识别的 VST3 效果器不能稳定打开其自身的原生 editor UI。
2. 首次勾选或取消勾选时 Qt 界面不会卡住，但实际声音效果要等待数秒才生效。

P9 的夹具和 MCP 流程只能证明测试 fixture 的窗口、请求队列和 buffer 写回路径可运行，不能证明任意第三方 VST3 的 controller/editor 合同或真实音频首个生效块满足要求。本计划以真实 Guitar Pro 宿主中的可观察结果作为交付门槛。

## 目标和边界

目标：

- 双击名称、上下文菜单和宿主桥接触发都能打开同一个 VST3 editor；窗口可见、可交互、可关闭和重开，且不破坏正在运行的音频链。
- 停用操作立即进入旁路；启用操作在准备完成后以一次受控提交切换，保留旧链直到新链可处理，消除无意义的停顿和重复初始化。
- 将“请求已接受”“插件准备中”“已提交到音频线程”“首个有效处理块”和“失败阶段”分别记录，避免把 UI 响应时间误认为音效生效时间。
- UI 只显示中性状态，详细阶段、VST3 result code、线程和耗时写入结构化状态/日志。

边界：

- 继续使用现有 Qt、VST3 SDK、双槽 `effects::Chain`、`SelectionSlot` 和宿主原生 HWND，不引入独立声卡流或新的后台服务。
- 只承诺 Guitar Pro 8.1.1.17 / Windows x64；其他版本仍由哈希门控旁路。
- 第三方插件自身的 `initialize`、`createView` 或状态恢复若耗时或不提供 editor，不能被强行修复；必须给出准确的结构化失败原因和可重复证据。

## 当前路径与待证实原因

| 问题 | 当前路径 | 风险/待证实点 |
| --- | --- | --- |
| editor 打不开 | `qt_ui.cpp::P7Panel::openEditor` 在 `g_vst3BusyControl()` 为 true 时每 50 ms 重试；`gp_hook.cpp::openVst3Editor` 使用 `try_to_lock` 查找活动 slot；`RuntimeEffect::openEditor` 依次执行 `createView("editor")`、平台检查、`setFrame`、`getSize`、`attached`。 | worker 忙时可能只得到布尔失败；真实插件可能需要 controller host interface、独立 HWND 条件或不同的 `IPlugView` 生命周期。当前状态没有把失败阶段和原始 result code 传回 UI。 |
| 首次启用慢 | global 请求由 selection worker 执行 `configureSelectedChain`，随后同一 worker 再执行 `configureInputSelection`；两条链可能各自加载 factory、创建实例、恢复 state 和 setup。 | UI 已经响应，但声音路径仍在等待冷启动；输入链停用时会销毁 warm instance。需要区分插件冷启动耗时和“准备完成后迟迟未提交”的耗时。 |
| 停用/再次启用 | 空 selection 最终调用 `chain.deactivate()`，输入链还会 shutdown；再次启用依赖 worker 重建或复用 slot。 | 停用没有明确的立即旁路提交语义，复用条件、generation 和 global/input 两条链的提交时序不够清晰。 |
| 现有验证不足 | `test-p7-mcp.ps1`、`test-p8-runtime.ps1` 主要使用内部 fixture 或显式 Qt host，`test-p9-switch.ps1` 验证双槽和 ramp。 | 尚未覆盖真实第三方 editor、真实双击路径、首次启停首个声音块延迟、快速反复切换和不同音频设备。 |

## 分阶段实施

### P10-0：建立可定位的基线

1. 在 global、track 和 live-input 请求中加入单调递增 `request_id`/`generation`，记录 `queued_at`、`worker_started_at`、每个实例的 factory/load、controller、state restore、`setupProcessing`、`setActive`、`setProcessing`、chain prepare、input prepare、activate、首个处理块和首个输出变化时间。
2. 为 editor 记录 `requested`、`busy_wait`、`controller_missing`、`create_view`、`platform_check`、`set_frame`、`get_size`、`attached`、`visible`、`focus`、`removed` 等阶段，以及原始 VST3 `tresult`。路径和 class identity 仅放结构化诊断，不放入中性 UI 文案。
3. 每次请求保留 `desired_selection`、`applied_selection`、`audio_generation` 和 `first_processed_sequence`；过期 worker 结果必须被丢弃，不能覆盖更新的用户操作。
4. 先用现有 fixture 建立 0 ms、500 ms、2000 ms 初始化延迟的基线，确认延迟发生在插件冷启动、输入链复制还是提交之后。

退出条件：能从单个 `status.json`/运行日志回答“用户何时点击、何时完成准备、何时切换、何时首个有效样本改变、失败在哪一步”，并且不需要猜测 worker 状态。

### P10-1：恢复真实 VST3 editor 打开链路

1. 将 editor 请求改为按 `scope + track_key + module + class_id + generation` 保存的待处理请求。插件准备中时只挂起该请求，由 worker 完成通知触发一次重试，移除固定 50 ms 轮询和无条件等待整个 selection worker 的做法。
2. 打开前确认活动实例仍匹配请求的 identity、state generation、采样率和 `ready`；查找失败时返回明确阶段，不把锁竞争、没有活动实例和插件没有 editor 混为一个 false。
3. 保证 Qt 线程创建并拥有稳定的 `NativeEditorWindow`、`WA_NativeWindow` 子 HWND 和 `RuntimePlugFrame`。在 `winId()` 产生且窗口可见后再调用 `attached`；窗口重建、GP 侧栏重建、DPI 改变和主窗口关闭都走同一套 attach/detach/resize 清理路径。
4. 审计并按真实插件需要补齐最小 host 合同：`IHostApplication`、`IPlugInterfaceSupport`、controller/component connection point、`IComponentHandler`，以及通过失败证据确认需要的消息或属性接口。`isPlugInterfaceSupported` 不能对未实现的接口一律返回 true。
5. 固定 editor 生命周期顺序：实例和 state 已恢复后创建 view，先 `setFrame`、content scale 和 size，再 `attached`；所有异常和非 `kResultOk`/`kResultTrue` 均保留阶段、result code 和清理结果。没有 `IPlugView` 或不支持 HWND 时只显示“原生 GUI 不可用”的中性状态。
6. global 与 track 共用同一 editor 窗口复用策略，但 editor 的持有对象必须跨 selector/sidebar rebuild 保活；关闭窗口只移除 view，不停用音频实例。

### P10-2：缩短首次启停的音频生效路径

1. 把“用户期望状态”和“当前音频提交状态”分离。取消勾选时先在音频原子状态设置 bypass，并在下一音频 callback 内可观察；清理实例、保存 state 和 UI 重排放到控制线程，保留可复用的已准备 slot。
2. 为 global playback chain 和 live-input chain 都保留 warm slot。若 module/class、component/controller state、采样率、block capacity 均匹配，启用只执行 `activate`/generation commit，不再次 `LoadLibrary`、创建 factory 或恢复 state；只有配置变化才进入冷准备路径。
3. 将 global 和 input 的准备阶段解耦：旧链在新链 ready 前继续处理；两条链均 ready 后一次发布同一 `audio_generation`。不能让 global 已切换而 input 仍默默等待数秒，也不能为了并行而在未知线程安全的第三方插件上强行并发调用。
4. 复用现有双槽 reader drain 和 ramp，但给控制线程的 drain 设置可观测的有界预算；超出预算时保留旧 slot、记录 `reader_drain_deferred`，由下一次控制 tick 重试，不能让 UI 线程等待。
5. 在 `Chain`/selection runtime 增加首个处理块确认：记录 activation 到 `process()`、到实际 output write-back、到 live-input output hash 改变的 callback 数和毫秒数。准备完成但首个块未在预算内出现时，状态必须指出是宿主没有回调、配置不匹配还是处理器回退。

### P10-3：统一 UI 状态和失败反馈

1. 复选框操作立即显示“请求已接受/正在准备”，停用立即显示“已旁路”；提交完成后更新为“已生效”，失败则恢复到实际 applied 状态并提供可重试入口。
2. editor 请求显示“准备完成后自动打开 GUI”，成功后显示窗口标题；失败只显示中性文案，详细 `editor_stage`、`tresult`、插件 identity 和 generation 保留在诊断数据。
3. 同一 scope 的快速连续勾选只保留最后一个 generation；旧结果不能把 UI 或 sidecar 回滚。准备期间仍允许打开 About、切换音轨和关闭 selector。
4. 保持现有稳定 `objectName`、窄侧栏和 DPI 行为；把“界面不卡”和“声音已生效”分别显示和统计。

### P10-4：回归、发布和回滚

计划新增或扩展以下验证入口（实施时再落地文件）：

- `native/test/p10_editor_test.cpp`：Qt thread、稳定 HWND、重复打开、关闭/重开、DPI、独立 controller、无 editor 和失败 result code。
- `native/test/p10_activation_test.cpp`：warm/cold 启用、立即旁路、global/input 同 generation、快速反复切换、采样率变化、reader drain 延迟和首个处理块。
- `native/test/test-p10-editor.ps1`、`test-p10-activation.ps1`、`test-p10.ps1`：串联夹具、MCP 宿主和真实 Guitar Pro 8.1.1.17 回归，证据写入独立 `artifacts/p10-*`。

测试矩阵至少包含：

- global 与 track；双击名称、上下文菜单和宿主桥接三种 editor 入口；selector/sidebar rebuild 后重开。
- 第三方插件 0/500/2000 ms 初始化延迟、独立 controller、single-component controller、无 HWND editor、状态恢复成功/失败。
- 44.1/48/96 kHz，64/128/256 frames，warm/cold 启用，启用/停用快速交替 20 次；同时观察 playback 和 live-input。
- 100%/125%/150% DPI、260/320/420 px 侧栏和真实扬声器/输出 callback 的首个变化证据。

## 验收门槛

以下指标以 20 次重复中的 P95 为准，冷启动耗时单独报告：

| 指标 | 门槛 |
| --- | --- |
| Qt 请求响应 | 复选框/打开请求返回并保持事件循环，控制线程单次阻塞不超过 100 ms。 |
| 停用生效 | 用户取消勾选后不超过 1 个宿主音频 callback 可观察到 bypass；不要求等待插件销毁。 |
| warm 启用 | 已准备实例从提交到 playback 和 live-input 首个有效处理块均不超过 2 个 callback 或 100 ms，取较早约束；无 sequence gap、fallback 或旧 generation 覆盖。 |
| cold 启用 | 从请求到“准备完成”的插件耗时完整记录；准备完成后不超过 2 个 callback 或 100 ms 生效。插件自身超过该时间的初始化不能伪装成音频提交延迟。 |
| editor | 请求对应的插件窗口可见、非 modal、尺寸至少 100×100、子 HWND 属于 editor host；重复打开复用同一 view，关闭窗口不停止音频。 |
| 失败可诊断 | 无 controller、无 `IPlugView`、HWND 不支持、host interface 缺失、state restore 失败和锁/代际过期分别有稳定 `error_stage`。 |

## 交付证据与未覆盖项

交付必须同时提供代码 diff、`git diff --check`、专项夹具结果、真实 Guitar Pro 宿主的 `status.json`/结构化日志，以及 editor 窗口截图或 native HWND capture。仅有“DLL 加载成功”“菜单可枚举”“请求返回 true”不能作为完成证据。

真实 ASIO/WASAPI 设备矩阵、不同 GP 版本、真实扬声器听感阈值和第三方插件进程级崩溃隔离仍属于宿主受限项；未运行的项目必须在实现记录中明确列出。若某插件没有合法 editor，交付结果应是可操作的无 editor 诊断和稳定旁路，不伪造插件 UI。

实施完成后新增 `docs/P10_IMPLEMENTATION.md`，并将本计划中的门槛、实际数值、失败样本和未覆盖项逐项回填。
