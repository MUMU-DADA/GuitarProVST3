# P12：首次启用到音频生效延迟优化计划

状态：计划中；尚未实施
关联文档：[P10 editor/音频生效优化计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)、[P11 UI 性能计划](P11_UI_PERFORMANCE_PLAN.md)
基线：Guitar Pro 8.1.1.17 / Windows x64

## 问题定义

用户在 VST3 面板勾选插件后，界面会立即显示请求已接受，但插件需要较长时间才真正影响声音。现有反馈表明该现象对多个 VST3 插件都存在，因此优先处理宿主侧的共同路径，再单独计量第三方插件自身的初始化耗时。

当前路径是：

```text
Qt 复选框
  -> publishSelection()
  -> requestGlobalVst3Selection()
  -> selection worker
       -> configureSelectedChain()
            -> LoadLibrary / factory / component / controller / state / setupProcessing
       -> configureInputSelection()
            -> 为 live-input 再准备一份实例
  -> Chain::activate() / input router enable
  -> Guitar Pro 下一次音频 callback
  -> 首个有效处理块和声音写回
```

UI 与音频准备已经异步分离，所以界面不卡并不代表声音已生效。当前 global 和 live-input 准备在同一个 worker 中串行执行；默认 `GPVST3_P4_ROUTE` 为 `InputInsert`，普通播放场景也可能为同一个插件创建第二份 live-input 实例。第三方 component/controller 初始化还需要通过 Qt 线程的阻塞调用完成。相关入口为 `native/modules/qt_ui.cpp`、`native/modules/gp_hook.cpp` 和 `native/modules/effect_chain.cpp`。

## 与常见宿主的差异

| 项目 | 常见宿主 | 当前实现 | 优化方向 |
| --- | --- | --- | --- |
| 初始化时机 | 工程加载、插件插入或后台预热 | 用户第一次勾选时冷启动 | 将准备移到启动/插入/空闲阶段 |
| 实例生命周期 | bypass 时继续保留 processor | 部分路径会重新创建实例 | warm slot 和实例缓存 |
| 启用动作 | 切换已准备好的 active/bypass 状态 | 勾选触发完整 selection prepare | 启用只做原子提交 |
| 播放链与输入链 | 只准备实际使用的路由 | global 后串行准备 live-input | 按路由懒准备、分别提交 |
| 模块资源 | 缓存模块和 factory | `RuntimeEffect` 初始化时重复获取 | 缓存 `HMODULE`/factory |
| 首个声音块 | 在运行状态下按 block 边界切换 | 还要等待准备和宿主 callback | 准备与 callback 延迟分开验收 |

插件自身的 `initialize()` 如果确实需要数秒，任何宿主都不能消除这段计算，只能把它提前完成或放到后台；“启用很快”主要来自启用动作只切换已经 warm 的实例。

## 优化目标

- cold start 的插件初始化时间单独记录，不把它伪装成提交延迟。
- 已 warm 的 global 或 track 链，从提交到首个有效处理块不超过 2 个 callback 或 100 ms，取较早约束。
- 播放链不因未使用的 live-input 链准备而等待。
- 停用在下一个音频 callback 内旁路，实例清理放到控制线程或延后回收。
- 采样率、block capacity、component/controller state 不匹配时明确进入 cold path，不能错误复用旧实例。
- 不在未知线程安全的第三方实例上强行并行调用；并行只用于独立模块或已验证安全的阶段。

## 分阶段实施

### P12-0：建立可比较的延迟基线

在现有状态字段基础上增加按 `request_id + generation + scope + module + class_id` 关联的请求记录，至少记录：

- `queued_at`、`worker_started_at`、worker 排队等待时间；
- module load、factory、component create/initialize、controller create/initialize；
- component/controller state restore、`setupProcessing`、`setActive`、`setProcessing`；
- global chain prepare/activate、input prepare/activate、router enable；
- 首个 `process()`、实际 output write-back、live-input 输出变化；
- 取消、过期 generation、reader drain、失败阶段和原始 VST3 result code。

当前 `snapshot()` 中的时间字段是全局最新值，连续操作时可能互相覆盖。应保留一个有界的最新请求记录或 ring buffer，保证同一请求的时间戳不会混用。

退出条件：一次复现即可回答延迟属于插件初始化、重复的 input 实例、worker 排队、Qt 线程等待、reader drain，还是宿主没有及时发送音频 callback。

### P12-1：先缩短播放链的关键路径

调整 `configureInputSelection()` 的触发条件：

1. 仅当 `stream.installed`、输入功能开启、路由不是 `Disabled`，并且 live-input 已经运行或被用户明确启用时才准备 input 实例。
2. global playback 链准备完成后立即提交 global `Chain::activate()`；input 链继续在后台准备，旧输入路由保持安全旁路或继续使用旧实例。
3. global、input 分别发布 ready/active 状态；整体 UI 状态不能让未使用的 input 路径覆盖播放链已生效状态。
4. 输入路径准备失败时只旁路 input，不回滚已经成功提交的 global playback 链。

这一步不改变第三方插件的线程合同，也不把同一实例同时交给两个可能并发的音频 callback。

### P12-2：建立 warm 实例和模块缓存

新增受控的 `PreparedInstanceCache` 或等价结构，按以下身份匹配：

```text
module + class_id + component_state + controller_state
       + sample_rate + block_capacity + route
```

实现要求：

1. `HMODULE`、factory 和已经完成 setup 的 processor 在控制线程生命周期内复用，避免每次重新执行模块发现和 factory 获取。
2. global playback 和 live-input 仍使用各自的 processor 实例，避免未知的第三方线程安全问题；缓存的是准备好的实例，不是跨路由共享同一个 processor。
3. bypass/取消勾选只断开 active slot，不调用 `RuntimeEffect::shutdown()`；实例在下一次配置不匹配或退出时再回收。
4. Guitar Pro 启动后优先预热保存过的链；打开 VST3 面板后可在宿主空闲时预热当前候选插件。
5. 预热队列必须有界，播放、编辑器打开和宿主关闭时暂停；不能默认一次性实例化所有已扫描插件。
6. 插件声明状态、采样率或 block capacity 不兼容时丢弃缓存并走完整 cold path。

### P12-3：拆分准备、提交和回收

将 selection worker 的工作拆成三个阶段：

1. **准备**：在非音频线程创建或复用实例，恢复状态并完成 processing setup。
2. **提交**：在 block 边界通过现有双槽 `Chain` 原子切换；旧 slot 继续保持有效，直到新 slot 首次成功处理。
3. **回收**：reader drain 和插件 terminate/setProcessing(false) 放到控制线程的延后回收队列，不让 UI 或音频线程等待。

同时缩小 `editorMutex` 的持有范围。插件实例的准备不应被无关 editor 请求长期阻塞；editor 自身仍使用每实例的生命周期锁和现有 Qt/HWND 合同。

`lockInputProcessing()` 和 `Chain::waitForReaders()` 需要增加可观测的有界预算。超过预算时保留旧 slot，记录 `reader_drain_deferred`，由后续控制任务重试，不能在 UI 中自旋等待。

### P12-4：UI 状态和宿主 callback 反馈

保留现有“请求中”文案，并补充结构化状态：

- `requested`：用户选择已进入队列；
- `preparing`：插件实例仍在 cold/warm 准备；
- `global_applied`：播放链已提交；
- `input_applied`：实时输入链已提交；
- `first_callback`：宿主已处理新 slot 的首个 block；
- `failed`：失败阶段、result code 和实际 applied selection。

如果宿主当前未播放或输入流未运行，UI 应显示“已准备，等待宿主音频回调”，不能把宿主没有 callback 误判为插件初始化失败。

### P12-5：回归与验收

扩展 `native/test/test-p10-activation.ps1` 或新增 P12 测试入口，覆盖：

- global、track、live-input 三种 scope；
- cold、warm、停用后再次启用；
- global-only、input-only、同时使用两条路由；
- 44.1/48/96 kHz 和 64/128/256 block；
- 第三方初始化延迟 0/500/2000 ms；
- 快速启用/停用 20 次、采样率变化和 editor 同时打开；
- 首个 callback、写回、输出变化、sequence gap、fallback 和 reader drain。

验收门槛：

| 指标 | 目标 |
| --- | --- |
| Qt 复选框响应 | 保持事件循环，单次控制线程阻塞不超过 100 ms |
| 停用旁路 | 不超过 1 个宿主 audio callback |
| warm 启用 | 提交后不超过 2 个 callback 或 100 ms 生效 |
| cold 启用 | 初始化耗时完整记录；准备完成后仍不超过 2 个 callback 或 100 ms 提交 |
| 未使用 input 路径 | 不创建实例、不拖延 global playback |
| 连续切换 | 无 sequence gap、旧 generation 覆盖或错误回滚 |

真实 ASIO/WASAPI 设备、不同 Guitar Pro 版本和第三方插件自身的初始化/编辑器阻塞仍需单独记录为宿主或插件边界，不能由 fixture 通过推导出完整能力。

## 当前临时规避

如果用户只需要 Guitar Pro 曲谱播放，不需要实时输入效果，可以在启动 Guitar Pro 前设置：

```text
GPVST3_P4_ROUTE=disabled
```

这会绕过 live-input 链的实例准备，代价是实时输入效果不可用。该设置只是诊断和临时规避，不替代 P12-1 到 P12-3 的正式实现。
