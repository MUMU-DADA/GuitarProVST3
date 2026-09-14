# P11：UI 周期卡顿逻辑去除与替换计划

状态：代码实现和专项入口已完成；真实 MCP 宿主双音轨回归通过，固定设备矩阵长时 CPU A/B 仍按宿主受限项记录（2026-09-15）。

基线：Guitar Pro 8.1.1.17 / Windows x64 / 当前工作树。正式 A/B 前固定源码版本、工作树差异和实际加载 DLL 的 SHA-256，避免把不同构建的结果混用。

关联文档：[运行逻辑与介入逻辑总图](PLUGIN_RUNTIME_AND_INTERVENTION.md)、[P10 计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)、[P10 实现记录](P10_IMPLEMENTATION.md)、[测试与验证](TESTING.md)。

## 1. 目标与处理原则

解决安装插件后 Guitar Pro UI 每隔零点几秒停顿的问题。优先去除空闲时的重复工作，将必要功能改为按变化触发；保留音轨绑定、故障旁路、采样率切换、选择结果同步和 editor 生命周期。

1. 移除固定 250 ms 全量音轨刷新和永久 500 ms 侧栏重挂载；扫描完成后停止 100 ms poll。
2. Qt 主线程只处理必须在其所属线程完成的宿主对象读取、控件更新和短时快照发布，不执行周期性文件序列化、同步写盘、reader drain 或整条 VST3 链重配。
3. 诊断快照改为只读；正常音频计数增长不再导致每 250 ms 替换一次 JSON 文件。
4. 复用现有 `ControllerObserver`、`NativeObjectRegistry`、selection worker、双槽和状态模块，不引入通用任务框架、新后台服务或独立音频设备。
5. 全量发现仅用于启动、已确认的宿主结构失效或显式恢复；发现过程要限量分批。延长全量扫描周期只能作为 A/B 隔离手段，不能作为最终修复。

本阶段仅调整控制与维护路径。第三方 VST3 自身必须遵守线程合同的初始化/editor 调用单独计时，不能通过随意换线程规避卡顿。

## 2. 已定位路径与证据边界

以下“已验证”指当前源码调用关系；运行时观察是前序排查样本，尚未完成各路径独立开关的 A/B 和调用栈耗时归因。

| 状态 | 入口 | 当前行为与问题 | 处理决定 |
| --- | --- | --- | --- |
| 已验证：代码路径 | `bootstrap.cpp::initialize` 中的 `trackTimer` | 每 250 ms 在 Qt 线程调用 `refreshTrackContext`，随后同步选择和音轨 UI。即使没有变化也进入发现路径。 | 拆分职责，替换为合并通知。 |
| 已验证：代码路径 | `gp_audio_runtime.cpp::collect/refresh` | 枚举 MCP bridge、遍历 `qApp`/`allWidgets()` 对象树和 registry，再枚举文档、音轨、音色；`refresh()` 每次都增加 generation。缺失音色时还可能执行 `musician->updateAll()`。 | 缓存已验证对象，按失效范围刷新；移除发现路径中的宿主强制更新。 |
| 已验证：代码路径 | `gp_hook.cpp::refreshTrackContextImpl` | 除发现外还承担失效实例退役、状态保存、故障恢复和 `reconfigureSlot`。仅把调用改成事件触发仍可能在 UI 线程长时间执行。 | Qt 收集上下文，worker 完成链维护与持久化，按 generation 发布。 |
| 已验证：代码路径 | `qt_ui.cpp::showEffectChainPanel` 中的 `g_panelAttachTimer` | 每 500 ms 查找 About、宿主区域和锚点并维护布局；隐藏面板时仍运行。`AboutEntryObserver` 还会对多种事件逐次排队 `singleShot(0)`。 | 用同一合并调度入口维护 About/侧栏，稳定后无重复挂载。 |
| 已验证：代码路径 | `vst3_autoload.cpp::writeObservation` → `state_manager.cpp::writeJson` | 每 250 ms 组装完整快照；变化时同步执行 JSON 序列化和 `QSaveFile`。现有快照相等比较包含持续递增的 callback counter。 | 区分状态与遥测，后台合并写盘。 |
| 已验证：代码路径 | `gp_hook.cpp::snapshot` | 会调用 `updateAudioLayerState()` 和 `reconfigureInputRouterIfNeeded()`；后者可能等待输入处理结束并调用插件 `reconfigure`。 | 将观测和控制分离，读取快照不再改变音频状态。 |
| 已验证：代码路径 | `vst3_autoload.cpp::initializePlugin` 中的 `scanTimer` | 每 100 ms 调用 `pollVst3`；扫描和识别结束后仍唤醒。 | 只在静态扫描或识别队列有任务时运行。 |

前序运行观察：Guitar Pro PID `52420` 的主窗口线程 `38772` 在约 2 秒内消耗约 `1609 ms` CPU，约为单核的 80%；同期被采样的音频输出线程 CPU 很低，`p2-observation.json` 约每 250 ms 重写。该样本与 UI 线程周期维护过重一致，但不能将 80% 全部归给某一个定时器，也不能推导为整个音频路径没有负载。正式基线需重新采样并保存证据。

## 3. 替换设计

### 3.1 音轨刷新：按变化收集，按 generation 发布

将现有 `trackTimer` 的职责分成三个独立入口，名称在实施时按现有 API 风格确定：

| 职责 | 触发条件 | 执行位置与范围 |
| --- | --- | --- |
| 当前文档/选中音轨更新 | 已验证的文档或选择信号、页面切换、相关对象重建 | Qt 线程读取缓存中的文档与 cursor，只更新当前 scope；不枚举全部音色，不重建未变化的音频链。 |
| binding 结构刷新 | 文档开关、音轨增删/重排、音色或 EffectsChain 重建、相关对象失效 | Qt 线程按受影响文档收集并发布不可变 binding 值；必要时分批完成。 |
| 链维护和 UI 结果同步 | selection worker 完成/失败、配置 generation 变化、chain fault、binding 发布 | worker 处理准备、保存、退役、重配；Qt 接收合并通知后更新对应面板和 pending editor。 |

调度与缓存约束：

1. 用小范围 dirty 位区分“当前选择”“binding 结构”“运行时配置/故障”。同一轮事件循环只排队一个处理请求；执行中发生的新变化保留到下一轮，不能在清理 dirty 时丢掉。
2. 先核实宿主实际可用的信号和生命周期。`ChildAdded`/`ChildRemoved` 不能证明所有曲谱模型变化都能被观察；按已验证的对象、selection model 或页面信号接入。对象构造/销毁 hook 中仅登记失效，延后读取完整对象，保持现有 Qt 哈希门控。
3. 复用 observer 与 registry 缓存；仅首次建立缓存或明确失效时遍历对象树。无关窗口、插件自己的布局与鼠标移动不得触发宿主全量发现。审查全局 event filter 的每事件成本，避免逐事件重复查找和分配。
4. 修正现有 generation 语义：结构或当前选择的有效值变化才递增；重读相同内容不能形成新一轮刷新。结构变化与选中音轨变化分别比较，选择变化不能遗漏，也不能导致所有 track runtime 重建。
5. MCP bridge 只提供已验证的文档/选择上下文；实时 `EffectsChain` 继续由 native collector 提供。保持 bridge 指针仅在回调内有效的边界，只复制 identity/context，不缓存 bridge 裸指针供异步或实时使用。
6. 当前 bridge ABI 的 generation 随枚举结果返回，不能假设已有便宜的 generation getter。只有验证其变化语义与访问成本后才用它失效缓存；不得为“检查 generation”再周期调用整套枚举，也不要求修改外部 GuitarProMCP 才能交付。
7. 移除 `collect()` 中因 sound 缺失而执行的 `musician->updateAll()`。先发布“上下文可用、chain 尚未就绪”，等待宿主完成构建后重试；若真实宿主证明必须主动更新，隔离为有明确条件、次数限制和耗时记录的恢复操作，不能放回日常发现路径。
8. QObject 及 GP 私有模型仍在允许的宿主线程访问；后台只接收值快照和已有安全生命周期机制保护的句柄。对象销毁先使对应 dispatch 失效，worker 的旧结果不得重新发布已销毁文档的 chain；保留现有 reader/generation 保护。
9. 启动与重建使用一次排队刷新；初始化尚未完成时可采用有限次数的退避重试。缺少可靠信号的路径允许低频兜底：初始间隔 2 秒，仅检查缓存对象存活和轻量签名，目标耗时不超过 1 ms；无变化时不枚举 bridge、不遍历全树、不写盘。不能有界完成时停止该兜底并记录限制。需要全量恢复时排队分批处理。
10. 仅靠兜底导致的音轨切换延迟不算正常路径达标，必须作为宿主受限项记录；正式移除旧 timer 前验证事件覆盖，避免用“最终会刷新”替代及时更新。

分批收集以约 4 ms 为每批主动让出预算，要求插件自身的单次 Qt 回调不超过 16 ms。若单个私有宿主调用已超过预算，继续拆批无法解决，需替换该调用或明确记录阻塞来源。

### 3.2 音频维护与选择完成：解除对诊断 tick 的依赖

1. 将 `refreshTrackContextImpl` 中的链准备、状态保存、故障隔离、退役和 rate reconfigure 移交现有 selection worker 的合并请求；保留 editor 与实例线程合同，避免在 Qt 持锁时等待 worker 回调 Qt。
2. worker 完成/失败后排队一次 Qt 通知，替代 `consumeSelectionStateChanges()` 的 250 ms 轮询。通知携带 scope/generation，旧结果丢弃；随后执行必要的 `reloadVst3Selections`、`syncVst3Selection` 和对应 track UI 更新。
3. `syncSelection()` 当前还负责 pending editor 重试。显式将其接到请求完成、失败、取消和对象失效通知，避免停掉 timer 后 GUI 永久挂起；保留 P10 的 scope/identity 检查和关闭/重开行为。
4. 实时 callback 只发布配置变化或 fault 原子标记，不创建 Qt 事件、不写文件、不等待控制锁。优先使用现有安全的非实时通知；缺少通知桥时，仅在相关链/输入路由活动期间由 worker 以有界短检查读取标记，初始上限 100 ms。该检查不调用 Qt、collector 或 JSON，空闲无任务时休眠。
5. `updateAudioLayerState()` 中需要宿主线程的轻量读取移到独立、按状态需要执行的入口；配置变化交给 worker。采样率、输入/输出通道、stream 生命周期和 chain fault 的通知覆盖要逐项验证，不能因为默认旁路时不触发就漏掉。
6. `hook::snapshot()` 仅读取已发布的原子字段/不可变状态和缓存；不调用宿主 getter，不修改 Router，不 drain reader，不调用 VST3，也不写盘。保留快照时间与 generation，读取竞争时返回可辨识的新旧快照状态。

这部分是移除 `trackTimer` 和诊断周期的前置条件：关闭诊断后，采样率切换、故障恢复、选择同步和 pending editor 必须仍正常工作。

### 3.3 侧栏与 About：合并事件，稳定后停止维护

1. 提取一次性的挂载函数，由启动、主窗口/侧栏创建、页面切换、相关祖先的 `ChildAdded`、`ChildRemoved`、`LayoutRequest`、`Show` 和对象销毁触发；复用现有 About observer，避免再增加第二套全局监听。
2. 将 `AboutEntryObserver` 当前“每个事件各排一个 singleShot”改成共享 pending 标记，同一批变化只运行一次。只关注宿主容器和相关祖先，忽略本插件挂载产生的等价布局事件；确保不形成“挂载 → LayoutRequest → 挂载”循环。
3. 用 `QPointer` 缓存主窗口、sound host、track/global anchor、section、toolbar 和 panel。先验证缓存及父子关系，失效才重新查找；有效布局不重复插入、设置固定尺寸或扫描 `allWidgets()`。
4. 挂载成功即停止调度。宿主尚未就绪时仅允许有限退避重试，例如 50/100/250/500/1000 ms；随后等待新生命周期事件或用户显式打开，不保留永久 500 ms 维护。
5. 面板隐藏且宿主稳定时不查找、不重建；再次显示时验证一次。启动开关关闭时仍保留 About 入口，不能间接启动 scanner、observer 或音频 hook。
6. 保留现有 `objectName`、展开状态、窄侧栏/DPI 行为、无重复按钮和 editor 宿主生命周期。事件合并只是延后到宿主完成本次重建，不得延后用户状态保存或使旧 QObject 被再次访问。

### 3.4 诊断：只读快照，后台合并写盘

1. 将状态变化和遥测分开：selection/editor/error/configuration/binding generation 触发状态更新；callback count、peak、时间戳等不参与状态变化判断。遥测需要时仍保留，不能为了减少写盘删掉 P10 音频生效证据。
2. 正常模式下，状态变化合并写入，频率上限初始设为每秒一次；只有计数变化时至多每 5 秒一次，完全无变化不写。250 ms 详细采样仅在显式诊断/回归模式中开启，并仍由后台序列化和写盘。配置名称在实施时确定，不把计划选项写成现有功能。
3. Qt 仅提交最小值快照；单一 writer 在后台构造 JSON、比较输出并执行 `QSaveFile`。用一个待写槽保留最新 generation，生产快于写盘时覆盖过期遥测，不积压无限队列；请求完成/失败的必要证据使用有界记录保留。
4. 同时处理 `status.json` 的扫描结果写入，避免把 `p2-observation.json` 移走后仍由 scan poll 同步写盘。同一路径只有一个 writer，旧快照不能覆盖新 generation；写盘错误保留在内存并限频重试，不能递归写错误日志。
5. 不全局异步化 `state_manager::writeJson`：`settings.json`、`effect-chain.json` 和 catalog cache 的保存确认、顺序和错误语义不同。由周期链维护引发的 sidecar 保存随控制工作移走；用户操作与退出时的持久化合同单独保留和验证。
6. 退出时先停止生产诊断任务，在依赖对象仍有效时捕获一次最终只读快照，再完成 writer drain 和清理。正常本地磁盘的最终 flush 目标不超过 1 秒，记录失败/耗时；禁止无限重试、退出后回调 QObject 或留下执行 DLL 代码的 detached writer。无需等待已销毁 Qt 事件循环来完成 writer。
7. 兼容现有 JSON 主要字段；增加采样模式、generation、快照时间和写入统计以判断新鲜度。P8/P9/P10 脚本显式启用所需诊断模式或读取内存状态，不能只延长测试超时掩盖功能退化。

### 3.5 扫描：任务活动时才 poll

1. 为 scanner 提供准确的“仍需 poll”状态，覆盖 static future、recognition queue 和当前 recognition job；`poll()` 返回 false 目前仅代表没有新 revision，不能据此停止 timer。
2. 静态扫描、识别和超时结算全部完成后停止 100 ms timer。识别等待期间仍需检查完成与 10 秒 deadline，不能在 static scan 完成时提前停掉。
3. 启动扫描、手动刷新和现有 60 秒维护扫描统一经过启动入口：先接受/合并扫描请求，再确保唯一 poll timer 活跃。扫描完成后再停；旧 generation 和已超时 worker 的迟到结果继续丢弃。
4. 保留当前后台发现、识别、缓存和超时策略。60 秒维护任务可暂时保留，但入口只能排队必要工作；审查 `beginAsync()` 的同步缓存读取和 `poll()` 的缓存/状态持久化，超预算部分移到扫描控制路径，避免每分钟出现一次大停顿。
5. 不支持的宿主、启动开关关闭、退出状态不得重新启动 poll。失败但尚未到 retry deadline 的条目不视为需要永久 100 ms 轮询的工作。

## 4. 分阶段实施与退出条件

实现与专项证据见 [P11 实现记录](P11_IMPLEMENTATION.md)。下表保留原验收目标；固定设备矩阵长时 A/B 与商业插件 editor 的覆盖边界在实现记录中单列。

| 阶段 | 改动范围 | 退出条件 |
| --- | --- | --- |
| P11-1：固定基线和测量 | 在现有回调边界增加耗时/次数、dirty 原因、遍历对象数、队列合并数、JSON 次数/字节统计；核实宿主事件覆盖。 | 能区分发现、链维护、挂载、快照和写盘耗时；测量数据先留内存、结束后导出，诊断自身无高频写盘。保存现状基线和逐项关闭的隔离结果。 |
| P11-2：替换 track tick 与控制副作用 | `bootstrap.cpp`、`gp_audio_runtime.cpp/.h`、`gp_object_registry.h`、`gp_hook.cpp/.h`、选择 UI 通知。 | 250 ms 全量刷新删除；选择/结构缓存正确失效；worker 承接维护；`snapshot()` 只读；关诊断后故障、rate、选择和 editor 流程仍通过。 |
| P11-3：替换侧栏维护 | `qt_ui.cpp` 的挂载入口、About observer、缓存和有限恢复。 | 500 ms 永久维护删除；稳定/隐藏窗口无重复挂载；事件突发可合并且不自激；重建/DPI/About 回归通过。 |
| P11-4：移走诊断写盘 | `vst3_autoload.cpp`、`bootstrap.cpp`、`state_manager.cpp/.h` 的诊断专用路径。 | Qt 无周期 JSON/QSaveFile；遥测不触发 250 ms 写入；队列有界、错误重试限频、退出最终快照有效；既有诊断脚本适配。 |
| P11-5：收敛 scanner poll | `vst3_autoload.cpp`、`bootstrap.cpp/.h`、`vst3_catalog.cpp`、`vst3_host.h`。 | 静态扫描/识别全部结束后 poll 次数不再增长；手动与 60 秒维护均可重新启动；超时/失败/迟到结果专项通过。 |
| P11-6：真实宿主与发布包验收 | P8/P9/P10 专项、性能采集和原有安装包验证入口。 | 达到下方性能门槛且功能无退化；记录实际 DLL、设备/曲谱/模式、原始数值和未运行项，再回填实现记录。 |

## 5. 性能验收门槛

下表是计划目标，不是当前测量结果。稳定窗口指启动扫描/识别结算后，宿主对象和选择不变；60 秒维护扫描活动窗口单独记录。

| 指标 | 初始门槛与解释 |
| --- | --- |
| 空闲重复工作 | 稳定窗口内，全量对象树/音轨/音色收集次数、侧栏重新挂载次数均为 0；有活动音频 callback 但没有上下文变化时也应满足。 |
| Qt 回调耗时 | 插件维护回调 P95 ≤ 4 ms，单次 ≤ 16 ms；同时记录总占用与事件队列延迟，不能用拆成大量小回调掩盖总负载。超出的私有宿主/第三方调用必须单列来源和时长。 |
| 主线程 CPU | 正常诊断模式、无启用链且稳定空闲时，主线程每 60 秒累计 CPU 相对“启动开关关闭”基线增加 ≤ 1.2 秒，即平均 ≤ 2% 单核；同时报告绝对值和相对未安装基线。 |
| UI 响应 | 固定的音轨选择、展开/收起和 About 操作重复至少 20 次，Qt 事件排队到处理的 P95 ≤ 50 ms；无需第三方准备的操作 P95 ≤ 100 ms，且无与 100/250/500 ms 周期对应的重复停顿。 |
| 上下文及时性 | 正常事件路径中，文档/音轨变化到 UI scope 与 binding 更新 P95 ≤ 100 ms；确认宿主信号缺失而走兜底的样本单列，不能混入通过值。 |
| 诊断写盘 | Qt 线程周期诊断序列化/写盘次数为 0；正常模式稳定播放时 observation ≤ 12 次/分钟，完全静止且计数无变化时为 0；详细诊断模式另报。 |
| scanner 空闲 | 没有 static future、recognition queue/job 时 100 ms poll 不再增长；维护启动/收尾的次数和时长可解释。 |
| 音频连续性 | 没有因本改动新增的 sequence gap、deadline overrun、fallback、配置错误或旧 generation 写回。bypass、warm/cold 首个生效块和 editor 生命周期仍满足 P10 已有验收边界。 |

主线程 CPU 使用固定线程 ID 的 CPU 时间差计算，报告为单核百分比；不要使用任务管理器按逻辑核数归一后的进程百分比替代。每个场景预热至少 30 秒，采样至少 120 秒，重复 3 次；维护扫描、真实播放和空闲分别切片统计。仪器结果与真实交互卡顿观察同时保留。

## 6. 验证矩阵与执行入口

### 6.1 新增或扩展的专项（计划入口，尚未创建）

- `native/test/p11_ui_performance_test.cpp`：事件合并、稳定期零扫描/零重挂载、结构失效和过期 generation、对象销毁/重建、隐藏面板、About、自触发布局事件、退出后的待处理通知。
- `native/test/p11_maintenance_test.cpp`：worker 通知不丢失、关诊断仍能处理 fault/rate/selection、snapshot 无副作用、写盘积压有界、最新快照顺序、写盘失败和最终 flush、scanner poll 停止与重启。按现有夹具组织，可合并文件，避免为了拆分而新增框架。
- `native/test/test-p11.ps1`：串联专项和真实宿主采集；fixture 只断言可重复的调度/次数/生命周期，真实耗时和卡顿门槛用真实宿主测量，避免把开发机计时抖动写成脆弱单元测试。

### 6.2 复用现有回归

按影响范围执行构建、PowerShell 语法检查和 `git diff --check`，再运行以下现有专项。具体 Qt、DLL、MCP、VST3 路径使用本机实际值，完整命令见 [测试与验证](TESTING.md)。

- `test-p8-track-runtime.ps1 -CheckLifecycle`：MCP context/native registry 与独立 native collector 两种绑定来源；多文档、音轨增删/重排、撤销、保存/另存/重开、音色重建和默认旁路。
- `test-p8-state.ps1`、`test-p8-ui.ps1`：identity 与 scope 持久化、侧栏重建、选择恢复。
- `test-p8-recognition.ps1`、`test-p8-recognition-timeout.ps1`：缓存、识别成功/失败/超时、迟到结果、手动刷新及 60 秒维护重启。
- `test-p9-ui.ps1`、`test-p9-switch.ps1`：About 幂等/启动开关、窄侧栏、100%/125%/150% DPI、快速切换和音频连续性。
- `test-p10-activation.ps1`、`test-p10-editor.ps1`：global/track/live-input，warm/cold、pending editor、双击/上下文菜单、关闭/重开、失败与取消、sidebar 重建时实例存活。
- `test-p6-package.ps1`：最终发布 DLL 和安装包验证；结果不能由开发目录中的另一份 DLL 代替。

### 6.3 真实 Guitar Pro A/B

先保存曲谱并正常退出宿主，使用现有安装/卸载与启动开关流程切换构建，不热替换已加载 DLL。各组固定宿主版本、曲谱、窗口/DPI、音频设备、采样率、VST3 目录和 MCP 状态，测试目录与用户配置分离。

| 组别 | 状态 | 目的 |
| --- | --- | --- |
| A | 未安装本插件 | 记录宿主及其他已装组件的基线。 |
| B | DLL 已安装，插件启动开关关闭 | 分离 autoload/About 的开销；不能用“总旁路”代替启动开关关闭。 |
| C | 启动开关开启，无启用链 | 覆盖用户最明显的安装后空闲卡顿场景。 |
| D | 一个 fixture 或已验证 VST3 已加载，总旁路 | 区分已有实例/维护成本和实际 DSP 成本。 |
| E | 启用一个测试 VST3 | 分别验证 global、track、live-input 和 editor 打开/关闭时的性能与声音生效。 |

对 C/D/E 比较当前基线 DLL 与候选 DLL；对 MCP 已安装/未加载两种配置分别记录，防止把 bridge 自身周期任务误归因给本插件。覆盖空曲谱与多音轨曲谱、停止/播放、折叠/展开面板、切换文档、音轨重建、采样率/设备变化、慢初始化和识别超时。

每组保存主线程 CPU 时间、插件回调次数与 P50/P95/max、事件队列延迟、dirty 原因、对象遍历数、JSON 次数/字节、scanner 状态，以及 audio callback 连续性和 editor 生命周期证据。P11-1 的逐项隔离只用于定位；最终候选必须在完整功能开启时满足门槛。

## 7. 风险、回退与交付

- **宿主受限**：GP 私有对象、Qt hook 和选择信号受精确版本与哈希门控约束。未验证事件覆盖、对象 lifetime 或 bridge generation 语义时不能承诺跨版本；缺少信号的降级行为和延迟必须单列。
- **失效风险**：缓存延长对象使用期；对象/文档销毁、地址复用、音轨重排和连续 generation 要验证，不能把已经失效的裸指针留在实时 dispatch。
- **线程风险**：移除 timer 会改变通知顺序；必须消除 Qt 与 selection/editor worker 的锁反转，保留安全的退役和退出顺序。第三方必须在宿主线程执行的调用单独处理和验证。
- **诊断风险**：异步低频快照可能使旧脚本读到陈旧状态；通过 generation/时间和显式诊断模式解决，不能重新启用正常模式的高频同步写盘。
- **回退方式**：每阶段单独保留可审查差异和通过的构建，失败时回退本阶段实现；真实用户临时停用使用既有启动开关并重启。A/B 隔离开关仅供诊断，交付时不保留多套永久调度器，也不默认恢复 250/500 ms 全量维护。

实施完成后新增 `docs/P11_IMPLEMENTATION.md`，逐项回填“已实现、已验证、实验性、未实现、宿主受限”、实际数值和失败样本，并更新运行总图、总览与测试入口。原始数据放在被忽略的 `artifacts/p11-*`，不提交用户配置、DLL、安装包或宿主副本。

本次交付包含代码差异、专项入口和真实 MCP 双音轨回归证据。性能门槛仍要求以固定宿主场景采样验证，不能仅由“DLL 加载成功”“菜单可见”或 fixture PASS 推导。
