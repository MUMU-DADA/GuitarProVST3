# P8：音轨级与全局 VST3 效果器、后台识别和顺序编辑计划

状态：已实现可验证范围（2026-09-11）；真实音轨映射保留宿主受限

当前锁定宿主可通过 `EffectsChain::index()` 提供链实例索引观测，但该字段不代表 GP 音轨 ID；在取得稳定的 `self -> track ID` 映射和可写 `IAudioBuffer` 边界前，音轨链继续旁路。

本计划处理四个新需求：启动后后台完成插件刷新/识别并更新缓存；区分音轨级和全局 VST3 链；启用项在列表前部显示；支持拖动排序且排序决定声音处理顺序。

## 当前实现和问题边界

- `vst3_catalog.cpp` 已能在后台做静态文件扫描和缓存，但缺少静态元数据的候选项目前会在用户勾选时通过 `g_identifyControl` 同步识别。该调用最终进入 `vst3::identifyBundle()`，会在 UI 触发 `LoadLibrary`、`InitDll` 和 factory 枚举，因此会卡住选择界面。
- 当前 `P7Panel` 只有一套 `effects_` 和一个 `effect-chain.json` 链；`gp_hook` 只有一套实际运行的 `SelectionSlot`，VST3 处理位置是 `Master::process` 返回后的 master 后处理点。
- `EffectsChain::processDSP` 当前只记录调用并转发到 GP 原函数，没有音轨标识到运行时链的映射。现有 `track` 字段只是 sidecar/UI 信息，不能证明实际作用域已是音轨级。
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
- 主动识别失败、超时或插件缺少可用 factory 时保留候选项并记录失败原因，不阻塞列表；启用动作只记录意图并等待识别结果，成功后自动提交，失败后保持未启用。
- P4 外部输入路由继续作为独立输入路径。第一版全局链定义为 GP master 混音链；是否把外部 capture 也纳入最终 global bus 另列为后续验证项，避免在未确认输出缓冲生命周期前改变监听语义。

## UI 设计

### 入口和作用域

继续使用 `soundsContainer` 中现有的 `VST3` 入口，避免猜测 GP 未公开的 Master 私有控件插槽。点击后打开同一选择面板，顶部使用两个稳定的 scope tabs：

- `当前音轨`：默认页，标题显示当前曲谱、音轨索引和可取得的音轨名称；GP 切换选中音轨后面板重新绑定该音轨的链。
- `全局 Master`：与选中音轨无关，显示作用于 GP 主混音的全局链。

稳定 objectName 规划为 `gpvst3ScopeTrackTab`、`gpvst3ScopeGlobalTab`、`gpvst3TrackChainList`、`gpvst3GlobalChainList`、`gpvst3AvailableList`。若 GP 后续提供稳定的 Master 区域入口，可增加快捷入口，但不改变两个 tab 的状态模型。

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
- 识别中的条目显示 `识别中…`，复选框不触发同步加载；识别成功后自动变为可启用项，失败显示原因并允许后台重试。
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

音轨 key 优先使用未来可取得的 GP 稳定 track ID；在没有稳定 ID 的首版使用 `score_key + track_index`，同时保存 `track_name` 作为诊断信息。音轨重排、曲谱切换和 key 不确定时不自动复用其他音轨的状态。

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
4. UI 勾选动作只更新 `desired_enabled` 和 scope 数组。如果条目仍在识别，立即返回并显示“后台识别中”；识别成功后由控制线程准备实例，失败则回滚为未启用并显示原因。
5. 手动刷新只提交一个合并请求：先做静态变化检查，再补充尚未完成的主动识别；不会因为用户重复点击而创建多个 loader 或识别任务。
6. 退出时停止接收新任务，取消队列并等待 worker 排空；不能在 UI 析构中同步调用第三方识别。

缓存新增字段：`recognition_status`、`recognition_source`、`recognition_attempts`、`recognition_error`、`recognition_retry_after` 和 `recognition_scanner_version`。静态结果和主动识别结果必须能在证据中分开计数。

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

- 新增单 worker 识别队列和取消/排空协议。
- 移除 `P7Panel` 勾选回调中的同步 `g_identifyControl` 调用。
- 识别进度、失败、重试时间和缓存写入状态进入 UI 快照。
- 保持静态扫描和主动识别的执行证据分离。

验收：冷启动、缓存命中、手动刷新、列表打开、识别进行中勾选均不阻塞 UI；同一 bundle 只识别一次；重启可复用识别缓存；关闭窗口和 GP 退出不遗留 worker。

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

- 将单一 `P7Panel` 拆为 scope tabs、启用列表和可用列表的共享模型。
- 拖动只改变当前 scope 的 enabled 数组顺序；保存、运行时发布和 editor key 使用同一 entry order。
- 刷新目录、切换音轨和后台识别完成时合并 catalog，不重置用户排序或启用意图。

验收：启用项总在前部；拖动 A/B/C 后运行时调用顺序为 A→B→C；停用/重新启用、重启和缓存刷新都保留顺序；global 与 track 列表互不污染。

### P8.6：真实宿主回归和文档交付

涉及 `native/test`、`docs/P8_IMPLEMENTATION.md`、`docs/INSTALL.md`、发布白名单。

- 新增后台识别、schema 迁移、两个 scope、track 切换、global/track 串联、启用项前置和拖动顺序回归。
- 在测试 VST3 中加入可观察的实例 ID、顺序标记和增益，证明同一插件在不同 scope 使用不同实例和 state。
- 真实 Guitar Pro 8.1.1.17 中验证启动识别、切换音轨、播放、停止、循环、保存重开、插件缺失和宿主哈希拒绝。
- 更新安装说明和实现记录，明确真实音轨映射、capture 监听和最终声学结果的宿主限制。

## 验收证据和完成标准

以下条件全部满足后才把 P8 标记为已完成：

- UI 线程不调用任何识别或插件加载函数；后台识别任务可取消、可恢复且缓存可跨进程复用。
- 静态扫描与主动识别分别有模块加载计数、时间、失败和缓存证据。
- schema 1→2 迁移通过，global/track state 和顺序在两个 GP 进程间保持。
- 至少两个音轨的实际 `processDSP` 调用能稳定映射到不同 track key；映射失败会旁路并报告原因。
- global、track、P4 input 三条路径的处理顺序有明确日志和可观察测试信号；未验证的相对顺序不写成已实现。
- 运行 `git diff --check`、适用的 C++/PowerShell/Qt 测试和锁定宿主回归；发布包不包含缓存、测试插件、临时宿主或 `artifacts/`。

真实扬声器听感、不同音频设备、ASIO/WASAPI 差异、GP 私有 `IAudioBuffer` 的完整所有权以及第三方插件崩溃恢复仍按宿主受限记录，不由本计划自动宣称完成。
