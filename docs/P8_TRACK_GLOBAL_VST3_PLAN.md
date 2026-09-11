# P8：音轨级与全局 VST3 效果器、后台识别和顺序编辑计划

状态：P8 六项补齐工作及交付验收已完成（2026-09-11，Guitar Pro 8.1.1.17 / Windows x64）。完整生命周期、实际列表挂载、独立 native 发现、无 MCP bridge 发布运行、P4 组合和发布包均有证据。逐项结果见 [P8 实现记录](P8_IMPLEMENTATION.md)。

当前锁定宿主的真实 `self -> track ID` 映射可由 GuitarProMCP bridge 或本 DLL 的独立 native collector 提供，可写 `IAudioBuffer` 已在双轨播放中验证。`EffectsChain::index()` 仅提供链实例索引诊断；个别未解析上下文仍旁路，不影响已绑定音轨。

本计划原有范围包括：启动后后台完成插件刷新/识别并更新缓存；区分音轨级和全局 VST3 链；启用项在列表前部显示；支持拖动排序且排序决定声音处理顺序。现根据宿主回归重新补齐六项未完成问题：识别中项目隐藏与 10 秒超时、音轨链真实可用、全局 UI 的母带后期处理位置、保留 GP 原生音源效果链、音轨 VST3 UI 的音源区位置，以及窄侧栏下的插件名称与布局适配。

## 六项补齐结果

六项原始问题均已有实现和针对性证据。下文保留实施要求；证据路径、实际数值与交付核对以实现记录为准。

| 编号 | 原始问题 | 补齐结果 |
| --- | --- | --- |
| 1 | 识别中隐藏和 10 秒硬截止 | **已实现/已验证**：真实 11 秒 factory、末项超时终态、队列推进、识别后 ready、缓存重启及手动重试 |
| 2 | 音轨链实际处理及生命周期 | **已实现/已验证**：双轨独立增益与写回、增删重排/撤销、首次保存/另存、关闭重开与全轨重启恢复；两种发现路径通过 |
| 3 | global UI 锚点及内容 | **已实现/已验证**：实际 global 列表、操作和分界线位于 `soundMastering` 后 |
| 4 | 保留 GP 原生音源效果链 | **已实现/已验证**：只插入自有 wrapper，不清空或替换宿主 layout；原生控件仍存在 |
| 5 | track UI 锚点及内容 | **已实现/已验证**：实际 track 列表、操作和分界线位于 `soundRack` 后 |
| 6 | 窄侧栏名称和操作布局 | **已实现/已验证**：左对齐、省略 tooltip、固定 GUI/拖动入口和高 DPI 最小高度通过 Qt fixture |

补齐阶段采用“宿主布局证据 + 运行时处理证据 + UI 回归”三类验收。仅有离屏 Qt 夹具、JSON 返回、菜单可枚举或 DLL 加载成功，均不能单独把上述问题标记为完成。

## 当前实现

- `vst3_catalog.cpp` 在后台完成静态文件扫描和缓存；缺少静态元数据的候选项由 `poll()` 放入单 worker 主动识别队列。用户勾选或打开列表时不再调用 `g_identifyControl`，`vst3::identifyBundle()` 及其 `LoadLibrary`、`InitDll`、factory 枚举都在后台执行。
- 两个固定 scope 的 `P7Panel` 复用行组件但分别持有 `effects_`，实际内容挂在两个宿主 section 中；global 和各轨的 `SelectionSlot`、参数、state 和 GUI 实例独立。
- `EffectsChain::processDSP` 使用 MCP bridge 或 native collector 的真实链映射，在 GP 原函数返回后执行对应音轨 VST3 链。逐轨证据进入 `track_runtime_evidence`；保存重开、增删重排和全部音轨自动恢复已通过。
- GP 运行时链使用 `am::overloud::Effect`，VST3 使用 `IComponent`/`IAudioProcessor`，不能把 VST3 实例直接塞进 GP 的私有效果器容器。音轨级实现应在 `processDSP` 的缓冲边界建立独立 VST3 链。

## 目标和明确边界

### 目标

1. Guitar Pro 启动后立即显示缓存清单，并在后台完成静态刷新；静态识别不足的 bundle 进入独立的后台主动识别队列。用户打开或操作列表时不执行同步识别。
2. 提供两个明确作用域：`当前音轨` 和 `全局 Master`。当前音轨链只处理该音轨的音频，全局链处理 GP 多轨混音后的 master 数据。
3. 每个作用域把已启用插件固定显示在上方，并显示“从上到下的处理顺序”。未启用插件显示在下方的“可用插件”区域。
4. 启用区域支持内部拖动排序；数组顺序、运行时 VST3 调用顺序和保存后的顺序完全一致。
5. 旧版单链配置可迁移且不改变已有声音意图：旧链按当前实际 hook 作用域迁移为 `global`，不猜测为某个音轨。

### 边界

- 静态扫描阶段继续保证不加载第三方模块、不创建第三方实例、不运行子进程、不联网。
- 为了消除“待识别项必须由用户点击后才识别”的阻塞，新增的“后台主动识别”是静态扫描完成后的独立阶段。它只在已通过宿主哈希门控的控制线程执行单 bundle factory 识别，不在 UI 线程或音频线程执行。该阶段可能执行第三方 `DllMain`/`InitDll`/`GetPluginFactory`，因此不能再把整个启动流程宣称为“第三方代码执行次数为零”；证据需分别记录静态扫描和主动识别两个阶段。
- 主动识别失败、超时或插件缺少可用 factory 时记录失败原因但不进入可启用列表；超时项在当前扫描周期直接忽略。启用动作只记录意图并等待识别结果，成功后自动提交，失败后保持未启用。
- P4 外部输入路由继续作为独立输入路径。第一版全局链定义为 GP master 混音链；是否把外部 capture 也纳入最终 global bus 另列为后续验证项，避免在未确认输出缓冲生命周期前改变监听语义。

## UI 设计

### 入口和作用域

继续使用 `soundsContainer` 中现有的 `VST3` 入口作为选择入口。实际 global/track 内容按 P8.10 分别挂到宿主锚点，通过 GP 原生音轨/曲谱侧栏页面显示各自区域：

- `当前音轨`：默认页，标题显示当前曲谱、音轨索引和可取得的音轨名称；GP 切换选中音轨后面板重新绑定该音轨的链。
- `全局 Master`：与选中音轨无关，显示作用于 GP 主混音的全局链。

稳定 objectName 为 `gpvst3TrackChainList`、`gpvst3GlobalChainList`、`gpvst3AvailableList`、`gpvst3GlobalAvailableList`、`gpvst3TrackVst3Section` 和 `gpvst3GlobalVst3Section`。早期选择面板的 scope tabs 已由两个固定作用域容器替代。

### 列表布局

每个 scope 使用同一套模型和行组件：

```text
搜索框                         [刷新状态]
当前音轨：Track 3 · Guitar

正在使用（从上到下为效果顺序）
  ☑ ParametricOD       ⋮⋮  [GUI]
  ☑ Gateway             ⋮⋮  [GUI]

可用插件
  ☐ NAM Rig             [GUI]
  ☐ Archetype ...       [GUI]
```

- “正在使用”区域只允许在同一 scope 内拖动；拖动后立即更新链数组并重新发布运行时链。
- 勾选可用插件会追加到启用区域末尾；取消勾选会移动到可用区域，不改变其他启用插件的顺序。
- 启用项始终在可选列表前列，启用项内部保持用户拖动顺序；未启用项按名称、厂商和稳定 entry key 排序。
- 识别中的条目不进入任何插件列表；识别进度只在状态栏显示，识别成功后才加入可启用项。失败和超时原因通过状态详情、日志和缓存记录查看，不能用一个不可操作的列表行占位。
- 当前音轨上下文不确定时显示“无法确认当前音轨”，禁止把某一音轨的链误用于另一音轨；全局页仍可用。

## 数据模型和迁移

### sidecar schema 2

`state_manager` 将 `effect-chain.json` 升级为按作用域保存的结构。全局链在应用数据目录中共享，音轨链按曲谱标识和音轨 key 保存：

```json
{
  "schema": 2,
  "global": {
    "effects": [
      {"entry_id": "module\\nclass_uid", "module": "...", "class_id": "...", "enabled": true, "order": 0}
    ]
  },
  "scores": {
    "score_key": {
      "tracks": {
        "track_key": {
          "track_index": 3,
          "track_name": "Guitar",
          "effects": []
        }
      }
    }
  }
}
```

每个 effect 保留现有的 `module`、`class_id`、名称/厂商、`enabled`、`component_state`、`controller_state`、参数和错误信息。`entry_id` 由规范化 module 路径和 class UID 组成，不能由显示名称组成。

sidecar 使用生成的持久化 track key；首次打开时按保存的唯一 `track_index` 关联记录，同一会话的增删/重排/撤销跟随原生 track ID。运行时 key 含 document ID，防止文档间共享实例；Save As 复制记录并复用现有实例。不能把进程内 UUID 当成跨进程永久 ID。

schema 1 的顶层 `effects` 全部迁移到 `global.effects`，因为现有运行时 hook 已证实是 master 后处理。迁移保留数组顺序、启用状态和 opaque state，并写入一次迁移标记；不删除旧字段直到 schema 2 成功提交。

### 运行时接口

扩展 `gp_hook` 的配置接口：

- `setGlobalVst3Selection(entries)`：发布全局 Master 链。
- `setTrackVst3Selection(trackKey, entries)`：为指定音轨准备并发布音轨链。
- `captureGlobalVst3States()` 和 `captureTrackVst3States(trackKey)`：保存各自实例的 state。
- 运行时状态增加 `global_chain_*`、`track_chain_*`、`track_context_*`、`track_scope_unresolved` 和每个链的处理计数，禁止用一个全局计数掩盖音轨作用域失败。

全局链和每个音轨链都使用控制线程预创建的双槽实例、预分配 planar 缓冲和原子发布。音频线程不能访问 Qt、磁盘、字符串 map 或阻塞锁。

## 后台刷新和识别流程

1. `bootstrap::initialize()` 读取缓存并立即发布可用清单；现有 `beginAsync()/poll()` 继续负责静态文件指纹、PE x64 检查、`moduleinfo.json` 解析和缓存更新。
2. 静态扫描完成或缓存命中后，`vst3_catalog` 构造 pending bundle 队列；同一时刻只运行一个主动识别任务。队列按已启用/sidecar 引用项优先，其余候选按稳定路径顺序处理。
3. 主动识别任务复用 factory-only 的 `identifyBundle` 逻辑，不创建 processor、不运行 `process()` 探针；每个 bundle 结束后通过快照通知 UI，并用 `QSaveFile` 更新对应缓存记录。
4. UI 只对已进入可启用列表的条目接收勾选；识别中的条目没有可点击行。sidecar 可以暂存 `desired_enabled`，识别成功后由控制线程准备实例，失败或超时则保持未启用并在状态详情中记录原因。
5. 手动刷新只提交一个合并请求：先做静态变化检查，再补充尚未完成的主动识别；不会因为用户重复点击而创建多个 loader 或识别任务。
6. 退出时停止接收新任务并清空队列，静态扫描 worker 正常排空；识别调用没有安全取消 ABI 时，超时 worker 继续 detached，退出流程不等待它，不能在 UI 析构中同步调用第三方识别。

缓存新增字段：`recognition_status`、`recognition_source`、`recognition_attempts`、`recognition_error`、`recognition_retry_after`、`recognition_deadline_at`、`recognition_ignored_reason` 和 `recognition_scanner_version`。静态结果和主动识别结果必须能在证据中分开计数。

## 音频接入和目标顺序

### 音轨级链

目标路径为：

```text
单个 RSE/MIDI 音轨
  → GP 自带 rse::EffectsChain::processDSP
  → 该音轨的 VST3 链（按拖动顺序）
  → GP 后续混音
```

`dspProcessHook` 先调用 GP 原函数，再在同一个 `IAudioBuffer` 上处理已解析到的音轨链。控制线程维护 `EffectsChain self` 到 `track_key` 的不可变 dispatch 表；音频线程只读取原子发布的表。

在启用音轨链前必须完成以下观测：`self` 指针与音轨上下文的稳定关联、音轨缓冲的 channel/frame/sample rate、同一块缓冲是否重复进入 hook、处理线程重入以及 GP 后续是否覆盖写回。任何一项不满足时只观测并旁路，不能把错误音轨的效果应用到当前音频。

### 全局链

第一版保持已验证的 master 后处理位置：

```text
所有 GP 音轨混音
  → GP Master::process 及 GP 主混音效果
  → 全局 VST3 链（按拖动顺序）
  → AudioLayer/PortAudio
```

当前 `GPVST3_P4_ROUTE=input_insert|bus_mix` 仍在 PortAudio callback 处理外部输入。若要求 global 链覆盖 capture 混音，必须另行验证最终 output buffer 的时序，并把 global 链移动到 P4 路由之后；在此验证完成前不改变既有 P4 语义。

### 不把两种 scope 混在一起

- 音轨链不得在 `Master::process` 中用最终混音缓冲模拟，否则会重复处理或把一个音轨的效果广播到全部音轨。
- 全局链不得根据当前选中音轨切换；它只使用一个 global runtime context。
- 同一插件可以同时出现在 global 和任意多个 track 链中，但实例、参数 state、旁路状态和处理计数彼此独立。

## 分阶段实施

### P8.1：后台主动识别和缓存

涉及 `vst3_host.*`、`vst3_catalog.cpp`、`bootstrap.cpp`、`qt_ui.*`。

- 新增单 worker 识别队列和停止/排空协议；当前识别控制没有安全取消 ABI，超时任务以 detached worker 方式回收，进程级 helper 隔离作为后续增强。
- 移除 `P7Panel` 勾选回调中的同步 `g_identifyControl` 调用。
- 识别进度、失败、重试时间和缓存写入状态进入 UI 快照。
- 保持静态扫描和主动识别的执行证据分离。

当前夹具验收：冷启动、缓存命中、手动刷新和列表打开均不阻塞 UI；识别中的 bundle 没有列表行；同一 bundle 只识别一次；重启可复用识别缓存；关闭窗口和 GP 退出不等待超时任务。由于识别控制没有安全取消 ABI，超时 worker 的 detached 计数会写入快照；进程级“无遗留线程”保证待 helper 隔离增强后补充。

### P8.2：schema 2 和作用域状态

涉及 `state_manager.*`、`qt_ui.*`、`gp_hook.h`。

- 实现 global/tracks 读写、schema 1 迁移和 opaque state 保留。
- 引入 `ScopeKind`、`ScoreKey`、`TrackKey` 和 entry key 校验。
- 让 UI 的保存、恢复、删除、旁路和 editor state 都带 scope。

验收：旧 sidecar 只迁移到 global；两个音轨的同一插件可保存不同 state；全局链不因切换音轨而改变；损坏或缺失的单个音轨记录不影响其他链。

### P8.3：音轨上下文发现和 processDSP dispatch

涉及 `gp_hook.cpp/.h`、必要的宿主观测脚本和状态快照。

- 在 `processDSP` hook 中建立 `self`、buffer、线程、序列和可取得 track context 的观测记录。
- 先实现只观测的 dispatch 验证，再启用单音轨测试链。
- 为每个 track context 使用固定容量双槽链和 reader drain；没有稳定映射时保持旁路。
- 维持 host hash/prologue 门控，并增加音轨作用域专用门控原因。

验收：至少两个 GP 音轨使用不同可观测增益插件时，处理计数、输出标记和声音能量分别对应各自音轨；切换选中音轨不会改变其他音轨链；未知 track context 不会误处理。

### P8.4：global Master 链整理

涉及 `gp_hook.cpp/.h`、`qt_ui.*`。

- 将现有单链命名和状态拆为 global context；保留双槽切换、旁路、错误回退和重配置逻辑。
- global 链仍在 GP Master 后处理点运行，直到最终 output callback 的跨路径验证通过。
- 明确 global 与 P4 input route 的顺序和状态字段。

验收：global 链对所有已渲染音轨一致生效；切换音轨不重建 global 实例；global 和 track 同时启用时处理计数和顺序标记符合目标路径。

### P8.5：双 scope UI、启用项前置和拖动排序

涉及 `qt_ui.cpp/.h`、Qt 夹具。

- 将单一 `P7Panel` 拆为两个固定 scope 的容器，复用启用列表、可用列表及行组件。
- 拖动只改变当前 scope 的 enabled 数组顺序；保存、运行时发布和 editor key 使用同一 entry order。
- 刷新目录、切换音轨和后台识别完成时合并 catalog，不重置用户排序或启用意图。

验收：启用项总在前部；拖动 A/B/C 后运行时调用顺序为 A→B→C；停用/重新启用、重启和缓存刷新都保留顺序；global 与 track 列表互不污染。

### P8.6：真实宿主回归和文档交付

涉及 `native/test`、`docs/P8_IMPLEMENTATION.md`、`docs/INSTALL.md`、发布白名单。

- 新增后台识别、schema 迁移、两个 scope、track 切换、global/track 串联、启用项前置和拖动顺序回归。
- 在测试 VST3 中加入可观察的实例 ID、顺序标记和增益，证明同一插件在不同 scope 使用不同实例和 state。
- 真实 Guitar Pro 8.1.1.17 中验证启动识别、切换音轨、播放、停止、循环、保存重开、插件缺失和宿主哈希拒绝。
- 更新安装说明和实现记录，明确真实音轨映射、capture 监听和最终声学结果的宿主限制。

## P8 补齐计划：六项回归问题（P8.7–P8.12）

P8.1–P8.6 作为已有基础继续保留。P8.7–P8.11 的识别、生命周期、原生链、实际分区域内容和窄侧栏已有验收，P8.12 汇总最终交付结果。以下保留原始要求，未知上下文旁路与已验证的真实音轨路径分开记录。

### P8.7：识别中隐藏和 10 秒超时

涉及 `vst3_catalog.*`、`vst3_host.*`、`bootstrap.*`、`qt_ui.*`、识别测试夹具。

- 目录快照只向 UI 提供 `recognition_status=ready` 且具有完整 audio effect 元数据的条目。`queued`、`running`、缺少 class UID、`failed` 和 `timeout` 条目不进入“正在使用”或“可用插件”列表，也不能被复选框或编辑器按钮访问。后台进度只显示在扫描状态栏和入口按钮中。
- 每个 bundle 从主动识别任务真正开始时计时，使用单调时钟设置 10 秒截止点；排队等待时间单独统计，不能因为排队而误算为插件已识别。
- 识别控制调用可运行第三方 `DllMain`/`InitDll`，不能在 GP 进程内强制终止任意线程。当前控制接口没有安全取消 ABI，因此 worker 在截止点标记超时后 detached，结果被丢弃，主线程立即回收任务状态并继续下一个 bundle；退出流程也不等待该 detached worker。不能用 `std::future` 析构等待一个已经超时的第三方调用。后续若需要进程级终止保证，再将 factory 识别迁移到受 watchdog 管理的隔离 helper 进程。
- 超时后写入 `recognition_status=timeout`、`recognition_error=recognition_timeout`、截止时间和尝试次数；当前扫描周期直接丢弃该候选，不能在下一次 `poll()`、列表刷新或面板重建时重新出现。手动刷新、bundle 文件指纹变化或扫描器版本变化才允许重新排队。
- 识别期间用户已有的启用意图可以保存在 sidecar，但不能创建实例、发布运行时链或在 UI 中显示为已启用；只有下一次得到 `ready` 才恢复该意图。
- 当前快照分别记录 worker 启动数、超时数和 detached 数，能够区分“未完成被隐藏”和“识别失败”；helper 进程及退出排空计数待进程级隔离增强后补充。

验收：用一个会睡眠 11 秒的测试 bundle 启动冷扫描；10 秒时 UI 仍可操作，列表中没有该条目，状态栏显示超时计数，队列随后可以继续识别下一个 bundle；关闭 GP 不等待超时任务；重启后超时项不会从缓存重新出现在列表。

### P8.8：音轨 VST3 从旁路变为可用链

涉及 `gp_hook.*`、`state_manager.*`、`audio_adapter.*`、宿主观测工具、测试 VST3 和真实 Guitar Pro 回归。

- 在锁定的 Guitar Pro 版本上继续以 `self`、调用线程、`IAudioBuffer` 地址、frame/channel/sample rate、调用序号和当前曲谱/选中音轨变化为证据，建立稳定的 `EffectsChain self -> track_key` 生命周期映射。`EffectsChain::index()` 只能作为诊断字段，不能单独当作音轨 ID。
- 映射建立前，音轨页面显示“音轨效果器暂不可用（等待宿主音轨上下文）”，隐藏启用复选框或将其置为不可操作；禁止把 desired state 当成已经生效的链。global 页面不受该状态影响。
- 映射建立后，为每个 `track_key` 预创建独立的 VST3 实例、双槽 runtime chain、planar scratch buffer 和处理计数；`processDSP` 先调用 GP 原函数，再只处理对应音轨的缓冲。未知、重复、重排、切换曲谱或生命周期失效的上下文必须旁路并记录原因。
- 音轨链的组件/controller state、bypass、顺序和编辑器实例与 global 完全隔离；同一插件出现在两个作用域时不得共享可变 processor 或参数状态。
- UI 只有在 runtime chain 已成功发布后才把音轨插件标为“正在使用”；创建失败、宿主拒绝和 buffer 边界不满足时回滚启用意图并显示可诊断原因。

验收：至少两个真实 GP 音轨分别加载带不同实例标记和增益的测试 VST3，播放时两条 `processDSP` 路径的 track key、实例 ID、处理计数和输出能量一一对应；切换选中音轨、停止/循环和保存重开不把链广播到其他音轨。若宿主仍无法提供稳定映射，专项记录必须明确“宿主受限，P8 未完成”，不能以旁路通过代替验收。

### P8.9：恢复并保护 GP 原生音源效果链

涉及 `qt_ui.cpp/.h`、GP 私有控件观测、Qt UI 夹具和真实宿主截图/对象树证据。

- 安装插件 UI 前先采集 `soundsContainer`、母带后期处理区域、原生音源区域及其 layout item 的对象名、类名、顺序、可见性和 parent；保存为宿主版本指纹，作为回归前后对比基线。
- 禁止对宿主 layout 调用 `clear()`、替换中央 widget、把原生音源 widget reparent 到插件容器，或把整块 `P7Panel` 盲目追加到 `soundsContainer` 末尾。重建时只能移除由本插件创建且带有 `gpvst3*` objectName 的 wrapper、separator 和 selector。
- 通过稳定 objectName/类名/相邻文本定位插入锚点；锚点找不到时保持插件区未挂载并报告原因，不能使用会挤掉原生控件的猜测位置。GP 重建侧边栏后重复挂载必须幂等，原生控件数量和顺序不能变化。
- 回归证据至少包含挂载前后原生音源 UI 的对象树、layout 顺序、geometry、visibility，以及原有音源效果链仍能打开、编辑、旁路和播放的结果。

验收：在同一 GP 进程中先确认原生音源效果链可见并启用，再打开本插件、切换两个作用域、刷新目录和切换选中音轨；原生音源控件不消失、不被覆盖、不被移到插件分界线之后错误的位置。

### P8.10：按宿主区域放置 global/track VST3 UI 并加分界线

涉及 `qt_ui.cpp/.h`、`P7Panel` 拆分后的共享模型、宿主控件锚点和 Qt/原生 UI 回归。

- 选择面板的数据模型可以共享，但渲染容器按作用域拆开，不能再用一个同时代表 global 和 track 的浮动/尾部面板解决布局。
- global 区固定插入“母带后期处理”区域的原生最后一个效果器之后，使用独立 wrapper 和水平 `QFrame::HLine` 分界线；建议稳定 objectName 为 `gpvst3GlobalVst3Section`、`gpvst3GlobalVst3Divider`、`gpvst3GlobalChainList`。
- track 区固定插入原生“音源/音轨本身效果链”之后，使用独立 wrapper 和水平分界线；建议稳定 objectName 为 `gpvst3TrackVst3Section`、`gpvst3TrackVst3Divider`、`gpvst3TrackChainList`。track 区的选择和编辑只作用于当前已确认的音轨。
- 两个区域都必须在 scope、曲谱、选中音轨和 GP 侧边栏重建后保持锚点；不能因为 global 页面打开而把 track 区挪走，也不能因为 track 上下文未知而隐藏或破坏 global 区。
- 分界线是结构性 UI 元素，不使用空白、边距或标题文字代替；布局顺序和父子关系写入 UI 夹具断言。

验收：真实宿主对象树能证明 global VST3 section 位于母带后期处理末尾、track VST3 section 位于原生音源效果链末尾；两条分界线可见且不会覆盖宿主控件。Qt 夹具和宿主回归都必须检查这两个相对位置。

### P8.11：窄侧栏下的名称和操作布局

涉及 `qt_ui.cpp/.h`、共享插件行组件、Qt offscreen 夹具和高 DPI/窄宽度回归。

- 所有插件行改为左到右的稳定布局：复选框/状态、可伸缩的名称区、厂商/状态副文本、拖动手柄、`GUI`/编辑器按钮。名称和副文本使用左对齐，禁止居中；名称区设置 `QSizePolicy::Ignored` 或等效策略，允许在窄宽度下优先获得可见空间。
- 名称过长时在行内使用省略号并提供完整名称、厂商、module/class UID 的 tooltip；不能通过把整行缩放到不可读来“适配”。启用列表和可用列表使用同一行组件，顺序和操作含义不能因宽度变化而改变。
- 侧栏宽度小于预设阈值时，厂商副文本可折叠为 tooltip，按钮改为固定宽度图标/短文本，但复选框、拖动和 GUI 入口必须保持可点、可键盘访问。高 DPI 下按 devicePixelRatio 计算最小高度和间距。
- 行、列表和外层 section 禁止强制固定大宽度；不要出现水平滚动条遮住名称或按钮。插件完整名称和当前状态应能通过可访问名称/API 查询得到。

验收：在 260、320、420 px 侧栏宽度及 125%/150% DPI 下，长名称仍左对齐且至少显示可区分前缀；完整名称可通过 tooltip 获取；所有复选框、拖动手柄和 GUI 按钮可操作且不重叠。

### P8.12：六项问题的联调、证据和交付

涉及 `native/test`、真实 Guitar Pro 8.1.1.17 回归脚本、`docs/P8_IMPLEMENTATION.md`、`docs/INSTALL.md`。

- 新增 `test-p8-recognition-timeout.ps1`、track runtime fixture、宿主布局快照比较、global/track 锚点断言和窄宽度 Qt fixture；现有 P8 state/recognition/UI 测试保留并扩展，不以修改断言来掩盖旁路。
- 联调顺序固定为：冷启动识别与超时 → 原生音源链基线 → global/track section 挂载 → 音轨 runtime chain → scope 切换/拖动/保存重开 → 窄宽度和高 DPI → 退出排空。
- 每个问题分别记录“已实现、已验证、实验性、未实现、宿主受限”。真实宿主没有证据的项目不得写入“已完成”；尤其是 track runtime 旁路、global 与 P4 capture 的相对顺序、第三方插件崩溃恢复继续单独标记。
- 交付前运行 `git diff --check`、受影响的 C++/PowerShell/Qt 测试、真实宿主回归和 `git status`；实现记录必须列出超时隐藏、原生链保留、两个 section 的相对位置、音轨实际处理和窄宽度结果。

P8 重新完成的最低证据是：识别中条目不可见且 10 秒后被忽略；至少两个音轨的 VST3 实际处理可区分；原生音源效果链仍存在；global/track UI 分别位于指定宿主区域并有分界线；窄侧栏的名称和控件可用。缺少任一项时，状态继续保持“P8 未完成”。

## 验收证据和完成标准

以下条件全部满足后才把 P8 标记为已完成：

- UI 线程不调用任何识别或插件加载函数；后台识别任务可取消、可恢复且缓存可跨进程复用。
- 静态扫描与主动识别分别有模块加载计数、时间、失败和缓存证据。
- schema 1→2 迁移通过，global/track state 和顺序在两个 GP 进程间保持。
- 至少两个音轨的实际 `processDSP` 调用能稳定映射到不同 track key；映射失败会旁路并报告原因。
- global、track、P4 input 三条路径的处理顺序有明确日志和可观察测试信号；未验证的相对顺序不写成已实现。
- 运行 `git diff --check`、适用的 C++/PowerShell/Qt 测试和锁定宿主回归；发布包不包含缓存、测试插件、临时宿主或 `artifacts/`。

真实扬声器听感、不同音频设备、ASIO/WASAPI 差异、GP 私有 `IAudioBuffer` 的完整所有权以及第三方插件崩溃恢复仍按宿主受限记录，不由本计划自动宣称完成。
