# P12：事件驱动切轨与按需运行时计划

状态：代码和专项验收完成；真实设备矩阵和其他宿主版本仍按宿主受限项记录（2026-09-16）。实现记录见 [P12_IMPLEMENTATION.md](P12_IMPLEMENTATION.md)。

基线：Guitar Pro 8.1.1.17 / Windows x64 / 当前 `master`。本计划处理三组相互关联的问题：

- 主界面尚未打开曲谱时仍发生 VST3 预加载和 Qt 主线程卡顿。
- 切换当前音轨依赖定时扫描，导致上下文不能及时更新或在运行一段时间后失效。
- 音轨面板使用旧 `trackKey` 时，现有音轨的启停可能失败或作用到错误 scope。

本计划参考 Cubase、FL Studio 等宿主的用户可见行为：插件扫描只产生 catalog metadata；工程加载只准备工程图中实际需要的插件；音轨选择只改变 inspector/editor scope，不重建整条音频图；新增或启用插件时只准备对应 scope。

## 1. 当前问题和源码证据

### 1.1 无曲谱时仍会预加载

`bootstrap::initialize()` 在主界面阶段已经设置了扫描、hook 和 track refresh 路径。catalog 交付后，`preloadSavedSelections()` 无条件把完整 ready catalog 放入 global 目标，即使当前没有打开曲谱：

- `native/modules/bootstrap.cpp::initialize`
- `native/modules/gp_hook.cpp::preloadSavedSelections`
- `native/modules/gp_hook.cpp` 中 `desired[""] = selections(global)`

因此当前主界面会创建全 catalog 的 global 实例。每个实例的 component、controller 和 `setupProcessing` 又可能通过 `invokeOnQtThreadBlocking()` 回到 Guitar Pro Qt 主线程执行。后台 worker 只把等待移出了调用者，不能消除第三方插件初始化对 Qt 事件循环的占用。

### 1.2 音轨选择没有可靠事件源

当前 `ControllerObserver` 只监听对象生命周期、`Show/Hide` 和动态属性变化，没有监听 `ScoreCursor` 的 track 变化。bridge 只在被调用时计算 `cursor().trackIndex()`，没有独立的 selection push/generation。

现有 2 秒 fallback timer 只适合作为启动恢复手段，不应作为长期选择同步机制；它使用静态计数，达到 30 次后停止，且不会因新曲谱或新文档重新计数。`refreshSelectionContext()` 在 bridge 暂时不可用或返回空时还会清除 dirty 状态，可能丢掉一次切轨事件。

### 1.3 selection context 和 audio graph 混在一起

当前 track maintenance 同时承担：

- 当前选中音轨和面板 scope 更新；
- document/track/EffectsChain topology 维护；
- TrackRuntime 准备、采样率重配、故障退役和状态保存。

纯粹的音轨选择变化不应该触发后两类工作。`sameBindings` 也不能用 `selectedTrack` 变化来伪装 topology 变化；应把 `binding_generation` 和 `selection_generation` 分开比较。

### 1.4 现有启停路径存在延迟窗口

音轨停用时的立即 bypass 使用 `try_to_lock`。如果预加载或 runtime maintenance 持有 selection lock，旁路提交可能被跳过，直到 worker 后续处理空 selection。同步的 `setTrackVst3Selection()` 还包含最长约 2 秒的等待和 `processEvents()`，不能作为 UI 路径。

### 1.5 现有验收没有覆盖新风险

现有 P8/P11 测试能证明最终实例和 buffer 结果，但没有把以下项目作为门禁：

- 无曲谱启动时的实例数量和 Qt 主线程最长阻塞；
- 真实用户切轨的事件延迟和连续 A→B→A 丢失率；
- 预加载期间停用的 callback 级旁路时序；
- 面板使用 stale `trackKey` 时的拒绝和自动重绑。

## 2. 目标和不变约束

目标：

1. 主界面无曲谱时只做静态 catalog 扫描，不创建 VST3 processor，不建立 track runtime，不启动音频 hook。
2. 打开曲谱时只准备工程配置中实际需要的 global/track 插件；未使用插件保持 metadata 或 sidecar 状态。
3. 音轨选择由宿主事件直接发布；切轨只更新 UI scope，不扫描对象树，不重建音频 dispatch，不重新初始化 VST3。
4. 新增/启用插件只准备对应 scope 的新 slot；旧音频链保持有效，ready 后一次性提交。
5. 停用先提交原子 bypass，插件销毁、状态保存和 UI 重排放到控制路径。
6. 保留现有 host hash/prologue gate、VST3 线程合同、双槽链、输入路由旁路和错误回退。

不在本阶段承诺：

- 其他 Guitar Pro 版本的私有 ABI 兼容；
- 第三方插件进程级崩溃隔离；
- 设备矩阵之外的真实听感阈值。

## 3. 事件驱动模型

```text
主界面 / 无曲谱
  └─ 只保留 catalog metadata 和 About/扫描入口

曲谱打开
  └─ document_opened
      ├─ 建立 score/track topology 和持久化身份
      ├─ 准备工程中已启用的 global/track audio graph
      └─ 发布当前 cursor selection

用户切换音轨
  └─ cursor_track_changed
      ├─ 发布 document_id + track_id + trackKey + selection_generation
      ├─ 立即切换 track panel/editor scope
      └─ 不触发 full collect、VST3 initialize 或 chain reconfigure

工程结构变化
  └─ topology_changed
      ├─ 增删/重排/关闭/重开时重建受影响 binding
      ├─ 保留未受影响 TrackRuntime
      └─ 仅为新增或启用 scope 准备插件

曲谱关闭
  └─ document_closed
      ├─ 停止对应 audio dispatch
      ├─ 后台保存状态
      └─ 释放或按 warm-cache 策略保留实例
```

selection context 和 audio graph 使用两套代际：

- `binding_generation`：document、track、EffectsChain、`self` 指针和 runtime identity 的结构变化。
- `selection_generation`：当前 UI 选中 track/document 的变化。
- `audio_generation`：实际已提交到音频 callback 的链变化。

纯 `selection_generation` 变化不能触发 `TrackDispatchUpdate`，也不能触发插件准备；只有 `binding_generation` 或用户显式修改链时才进入 runtime maintenance。

## 4. 切轨 hook 方案

### 4.1 候选节点

优先验证以下 Guitar Pro 8.1.1.17 节点：

1. `ScoreCursor::moveToCursorAndNotify(...)`：名字表明它可能是游标移动和通知的汇聚点，优先作为主候选。
2. `ScoreCursor::trySetTrackIndex(...)`：覆盖显式设置 track index 的路径，作为补充候选。
3. 当前文档/页面切换对应的 `IDocumentsManager` 或 Qt stacked-page `currentChanged` 路径。
4. 音轨增删重排对应的 `Score::createTrack/duplicateTrack/removeTrack/swapTracks` 路径，只产生 `topology_changed`。

这些方法目前只能视为候选。实施前必须通过真实 Guitar Pro 调用计数和 prologue 采样证明：鼠标点击、键盘移动、MCP `gp_cursor`、文档切换和重建路径分别经过哪些节点。

### 4.2 hook 合同

hook 只允许：

- 调用原函数；
- 在确认的 Qt/宿主控制线程读取轻量 cursor/document identity；
- 发布不可变 selection snapshot；
- 合并排队一次 UI 更新。

hook 不允许访问 sidecar、遍历 `allWidgets()`、加载 VST3、等待 selection mutex 或执行音频链重配。节点定位失败、prologue 不匹配或线程不符合合同时，保留旁路并回退到短暂恢复窗口。

### 4.3 bridge 协同

`GuitarProMCP` bridge 应增加可观察的 selection generation 或事件转发；当前 `enumerateAudioBindingsV1` 只在消费者主动枚举时读取 cursor track index。bridge 事件用于减少扫描和测试不代表插件可以绕过自身的 host hash/prologue gate。

## 5. 按需实例化和 warm-cache

### 5.1 三层职责

| 层 | 启动/打开时行为 | 是否创建 VST3 实例 |
|---|---|---|
| Catalog | 扫描路径、metadata、class id、识别状态、缓存 | 否 |
| Project graph | 读取 global/track sidecar，建立 enabled/bypass/order/state 描述 | 只为实际启用条目创建 |
| Audio graph | 在 worker 准备 slot，按 scope 提交 callback | 是 |

未打开曲谱时，global 也不能因为 catalog 存在而创建所有插件实例。打开曲谱后，只准备该曲谱中实际启用的 global 和 track 链；未启用、未配置、仅出现在 catalog 的插件不预加载。

### 5.2 当前选中音轨和音频音轨的关系

播放时多条已启用音轨仍需各自处理，因此“只准备当前选中音轨”不能成为音频策略。正确做法是：

- 所有工程中已启用的 track scope 按需准备并保持自己的 runtime；
- 当前选中音轨只决定 panel/editor/sidecar scope；
- 切换选中音轨不影响其他音轨的 audio graph。

### 5.3 warm-cache 规则

- 停用先 bypass，保留实例由 scope cache 管理；
- 同一 module/class、state、sample rate、block capacity 匹配时复用；
- cache 有实例数量和内存上限，超限按最近使用 scope 淘汰；
- 预加载 worker 可在插件之间让出控制权，但不能在单个插件的 Qt 合同调用中强行打断；
- 识别失败、state restore 失败和 setup 失败只隔离当前条目，不阻塞其他 scope。

### 5.4 状态语义

将用户意图和当前进程状态分开：

- sidecar：用户明确配置的 `enabled/order/state`；
- runtime：`desired/applied/bypassed/preparing/failed`；
- 新进程初始是否旁路由独立启动策略表达，不能靠每次启动覆盖用户 sidecar 的 `enabled` 字段。

历史 `desired_enabled` 数据需要一次迁移规则和回归样本，不能在预加载实现中隐式解释为“立即发声”。

## 6. 分阶段实施

### P12-0：真实宿主基线和 hook 侦测

- 在 Guitar Pro 8.1.1.17 中记录候选 cursor/document/topology 节点调用次数、线程和参数结果。
- 增加 `selection_event_source`、`selection_generation`、`binding_generation`、`context_publish_latency` 和 `dropped_refresh_count` 诊断字段。
- 记录主界面无曲谱时的 Qt CPU、VST3 instance count、preload queue depth 和主线程最长阻塞。
- 固定实际加载 DLL SHA-256、宿主文件完整性和用户曲谱副本。

退出条件：能区分“没有事件”“事件到达但发布失败”“UI 绑定 stale”“audio graph 未提交”四类原因。

### P12-1：实现事件驱动 selection context

- 优先实现 `moveToCursorAndNotify` 候选验证；必要时补 `trySetTrackIndex`。
- 增加 document active/closed 事件和短暂重建 settling 状态。
- `gp_audio` 发布 selection snapshot；UI 只消费 snapshot，不主动 full collect。
- refresh worker 获取锁失败时保留 dirty reason 并重新排队。
- 将 2 秒 fallback 限制为 document open/rebuild 后的有界恢复窗口，成功收到事件后停止；不再用静态计数永久停止。

### P12-2：拆分 selection 和 audio graph

- `refreshTrackContextWorkerImpl()` 只在 topology/configuration generation 变化时维护 TrackRuntime。
- 纯 selection 变化只更新 `state::runtimeTrackContext`、panel scope 和 editor pending key。
- UI action 携带 `selection_generation`；generation 过期时拒绝旧请求并自动重绑当前 scope。
- 音频 callback 继续根据已发布 `self -> TrackRuntime` 表处理，不依赖当前 UI 选中音轨。

### P12-3：移除无曲谱预加载和全 catalog 实例化

- 主界面保持 metadata-only；不调用全 catalog `preloadSavedSelections()`。
- 曲谱打开后只准备该工程实际启用的 global/track entries。
- 显式启用、停用、排序和参数编辑均由 scope worker 处理；旧链 ready 前保持旧链或旁路。
- 引入有上限的 warm-cache 和实例淘汰；输入路由继续默认 disabled。

### P12-4：UI 和 catalog 调度收敛

- 扫描/识别结果按批次或短暂 idle 合并更新 UI，不为每个模块重建两套列表。
- catalog cache 写入使用后台 latest-slot writer；Qt timer 只读取完成状态。
- 无曲谱时不创建 track panel；曲谱打开后再挂载和绑定 track section。
- 删除同步 selection API 中的等待和 `processEvents()`；所有 UI 操作统一返回 queued/applied/failed 状态。现有 `setTrackVst3Selection()` 仅保留兼容别名，真正完成状态通过 worker 诊断发布。

### P12-5：回归和发布

- 增加 cursor hook 单测/夹具：mouse、keyboard、MCP、文档切换、曲谱重建和 hook gate 失败。
- 增加 selection context 单测：generation、stale key、锁竞争重试、纯切轨不重建 audio graph。
- 扩展 MCP 宿主回归：无曲谱主界面、打开/关闭曲谱、A→B→A 20 次、快速启停、播放中切轨、增删重排、保存重开。
- 增加冷启动矩阵：0/2/10/20 个 catalog plugin、0/2/4 条已配置音轨、商业插件和 fixture。

## 7. 验收门槛

| 指标 | 目标 |
|---|---|
| 无曲谱主界面 | VST3 runtime instance 为 0；不执行 track collect/preload；音频 hook 可保持未安装 |
| 曲谱打开 | 只创建工程中实际配置的 global/track 实例；未启用 catalog 不创建 processor |
| 切轨上下文 | A→B→A 20 次无丢失；selection context P95 ≤ 100 ms；不触发 VST3 initialize |
| 音频连续性 | 纯切轨无 `audio_generation` 变化、无 sequence gap、无旧 generation 覆盖 |
| 停用 | 用户操作后 ≤ 1 个 audio callback 可观察到 bypass |
| warm 启用 | ready 实例提交到首个有效处理块 ≤ 2 个 callback 或 100 ms |
| 冷启用 | 插件自身初始化耗时单独记录；准备完成后提交仍满足 warm 门槛 |
| stale scope | 旧 `trackKey` 请求被拒绝并自动重绑，不得修改另一条音轨 |
| UI 卡顿 | 控制路径单次阻塞 P95 ≤ 100 ms；预加载期间不出现连续主线程长阻塞 |
| 资源上限 | 实例数、预加载队列和 warm-cache 占用有明确上限，超过后可观测淘汰 |

## 8. 风险、回滚和宿主边界

- `ScoreCursor` 私有方法可能内联、被不同模块实现或在其他 Guitar Pro 版本改变；所有 hook 必须保留 prologue/hash gate 和自禁用路径。
- 某些第三方插件的 component/controller 初始化必须在 Qt 线程，不能靠换线程消除插件自身耗时；只能通过懒加载、分批提交和不阻塞用户动作降低影响。
- bridge selection generation 需要与 `GuitarProMCP` 同步发布；bridge 不可用时仍要有 native hook 或安全旁路。
- 过渡期保留实验开关：事件 hook、lazy project graph、legacy full preload 分别可独立关闭；默认发布路径使用事件驱动和 lazy graph。
- 真实 ASIO/WASAPI、其他 Guitar Pro 版本、第三方插件崩溃隔离和设备听感仍按宿主受限项记录。

关联文档：[P8 音轨/global 范围](P8_TRACK_GLOBAL_VST3_PLAN.md)、[P11 UI 性能计划](P11_UI_PERFORMANCE_PLAN.md)、[运行逻辑总图](PLUGIN_RUNTIME_AND_INTERVENTION.md)、[测试与验证](TESTING.md)。
