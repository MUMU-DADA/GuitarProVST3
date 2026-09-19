# GuitarProVST3 运行逻辑与介入逻辑总图

本文按当前工作树绘制插件从加载、校验、扫描、选择、实例准备、实时处理、原生 editor、状态持久化到退出的完整路径。图中的“当前实现”以 `native/` 源码为准；计划文档中尚未落地的行为不画成已实现能力。

## 阅读约定

- **控制线程**：Qt 主线程、扫描线程和 selection worker，负责文件、VST3 factory、实例创建、状态恢复、UI 和链交接。
- **实时线程**：Guitar Pro 的 Master、音轨 DSP、PortAudio callback。只读取已发布的实例/参数/缓冲区，不创建 Qt 对象、不扫描磁盘。
- **editor 线程**：每个打开的 VST3 view 使用独立持久线程执行可能阻塞的 editor contract；Qt 主线程继续泵事件。
- **实线**：音频或控制数据实际流转；**虚线**：诊断、状态或证据流。
- `global`、`track`、P13 `input` 是三套独立范围。下文原有 live-input copy 图仅描述显式 P4 兼容路由，它是 global 选择的独立实例副本；P13 不参与该参数/state 镜像。

## P13 独立输入入口

普通构建新增“输入 VST3”窗口和独立 `input` 持久化范围。ASIO 生命周期代理提供 generation、rate revision 与实际 rate；`MonitorExchange` 在块边界发布槽位、原生监听分流和故障状态。listener 接收原 capture 并继续维护 DSP/电平，其末端输出进入独占 sink；共享 output SRC/ring 按有限历史排空后，input VST3 的结果才叠加到原 GP/RSE output。RSE 的原有 Master/track/global 顺序保持。

```text
真实 ASIO capture → 独立 input VST3 → 输入增益 ─────┐
RSE → 原生音轨/track VST3 → Master/global → SRC ──┴→ 设备输出
原生 input listener → 独立 sink（保持 DSP、电平更新）
```

首次准备失败保留原路由，已激活后处理失败丢弃输入贡献；关闭时停止 overlay 并恢复原生监听路由。新流重新核对合同，受限流不继承旧抑制。普通构建只接受已经核对的 192000 Hz ASIO 与 GP 内部 44100 Hz SRC 拓扑；真实设备范围、尾音/逐样本/延迟和发布证据见 [P13 实现记录](P13_IMPLEMENTATION.md)。以下 P4 图示中的 global→input 同步与 dry fallback 不适用于该模式。

## 1. 系统边界与三条介入面

```mermaid
flowchart LR
    GP["GuitarPro.exe\n8.1.1.17 / Windows x64"]
    AUTO["Qt imageformat autoload\nvst3_autoload.dll"]
    UI["Qt UI\nP7Panel / About / editor host"]
    STATE["state_manager\nsettings.json / effect-chain.json\nstatus.json / p2-observation.json"]
    SCAN["vst3_catalog\n静态发现、指纹、缓存、识别队列"]
    HOST["vst3_host\nfactory / component / controller / probe"]
    HOOK["gp_hook\n版本门控、函数 patch、运行时分发"]
    CHAIN["effects::Chain\n双槽、reader drain、ramp、故障旁路"]
    ADAPTER["audio_adapter\nPlanarBuffer / BlockView / VST3 ProcessData"]
    ROUTER["input::Router\nInputInsert / BusMix / interleaved 转换"]
    GPAUDIO["gp_audio_runtime\nQt 对象注册、文档/音轨绑定表"]
    VST["第三方 VST3 bundle\ncomponent / processor / controller / IPlugView"]
    MASTER["GP Master::process\n原生 Master 后处理点"]
    DSP["GP EffectsChain::processDSP\n原生音轨链后处理点"]
    STREAM["AMAudio PortAudio stream callback\n原始设备输出回调后"]

    GP --> AUTO
    AUTO --> UI
    AUTO --> HOOK
    AUTO --> SCAN
    UI <--> STATE
    SCAN <--> STATE
    SCAN --> HOST
    HOST --> VST
    UI --> HOOK
    GPAUDIO --> HOOK
    HOOK --> CHAIN
    HOOK --> ROUTER
    CHAIN --> ADAPTER
    ROUTER --> ADAPTER
    ADAPTER --> VST

    MASTER -->|hook 后调用原函数，再执行| HOOK
    DSP -->|hook 后调用原函数，再按 self 查找 track runtime| HOOK
    STREAM -->|hook 后调用原函数，再按当前配置处理输入| HOOK
    HOOK -.-> STATE

    classDef host fill:#e8f1ff,stroke:#356ae6,color:#102a56;
    classDef control fill:#eaf8ee,stroke:#2e8b57,color:#143d25;
    classDef audio fill:#fff1df,stroke:#c27619,color:#5b3505;
    classDef diagnostic fill:#f2eaff,stroke:#7b4ab1,color:#381a57;
    class GP,MASTER,DSP,STREAM host;
    class AUTO,UI,STATE,SCAN,HOST,GPAUDIO control;
    class HOOK,CHAIN,ADAPTER,ROUTER,VST audio;
```

### 介入点清单

| 介入层 | 当前入口 | 介入位置 | 作用 |
| --- | --- | --- | --- |
| 加载介入 | `GuitarProVst3Autoload` | Qt imageformat plugin 构造函数 | 只在进程名为 `GuitarPro` 时排队初始化，并用 `gpvst3P0Scheduled` 防重复 |
| Master 音频介入 | `masterProcessHook` | `Master::process` 原函数返回后 | 读取 GP `AudioBuffer`，执行 global chain，失败时写回旁路数据 |
| 音轨音频介入 | `dspProcessHook` | `EffectsChain::processDSP` 原函数返回后 | 用 `self` 查找已发布的 track runtime，执行对应音轨链 |
| 设备输入介入 | `streamCallbackHook` | PortAudio 原 callback 返回后 | 读取 interleaved input/output，执行 live-input 路由并写回设备 output |
| 音轨上下文介入 | `gp_audio::initialize/refresh` | Qt event filter、Qt 对象注册回调 | 发现文档、音轨、`EffectsChain` 和当前选中音轨，发布稳定绑定表 |
| UI 介入 | `P7Panel` / `About` | GP 原生侧栏和工具栏 | 选择、排序、旁路、参数保存、editor 入口、启动开关 |
| editor 介入 | `RuntimeEffect::openEditor` | Qt HWND 子窗口 + editor worker | 建立 VST3 `IPlugView`，不停止音频实例 |

## 2. 从进程启动到退出的全链路

```mermaid
sequenceDiagram
    participant GP as GuitarPro.exe
    participant Qt as Qt/QImageIOPlugin
    participant Boot as bootstrap
    participant State as state_manager
    participant Hook as gp_hook
    participant Catalog as vst3_catalog
    participant UI as P7Panel/About
    participant Audio as GP 音频回调

    GP->>Qt: 加载 imageformats/guitarpro_vst3_autoload.dll
    Qt->>Qt: 检查进程名、单次调度、singleShot(0)
    Qt->>Boot: initialize()
    Boot->>State: pluginEnabled()
    alt 启动开关关闭
        Boot->>UI: 清空控制回调、扫描状态=disabled
        Boot-->>Qt: 只写 status，退出初始化
    else 启动开关开启
        Boot->>State: disableAllEffectsAtStartup()
        Note over State: 将持久化 enabled 清为 false，保留 desired_enabled 和 state bytes
        Boot->>Hook: gp_audio.initialize(); hook::prepare(host)
        Hook->>Hook: host hash + export + prologue gate
        Hook-->>Boot: installed / bypass / reason
        Boot->>Hook: refreshTrackContext()
        Boot->>UI: 注入 selection、editor、旁路、扫描回调
        Boot->>Catalog: beginAsync(hostSupported)
        Boot->>Qt: 启动按需 scan poll(100ms) 与事件合并刷新
        Qt->>UI: showEffectChainPanel(false)
    end
    loop 运行期间
        Qt->>Catalog: pollVst3()
        Qt->>Hook: refreshTrackContext()
        Qt->>State: writeObservation()（状态变化时替换文件）
        Audio->>Hook: Master / DSP / stream callback
    end
    GP-->>Qt: aboutToQuit / post routine
    Qt->>Catalog: shutdownScan()
    Qt->>UI: shutdownEditors()
    Qt->>Hook: hook::shutdown()
    Qt->>Hook: remove patch、旁路、deactivate、销毁实例
    Qt->>State: 最终写入 p2-observation.json
```

## 3. 启动门控与 hook 安装

```mermaid
flowchart TD
    A["initializePlugin()"] --> B["bootstrap::initialize()"]
    B --> C["host::verify()\n校验 GuitarPro.exe / GPCore.dll / GPRSE.dll / AMAudio.dll / AMOverloud.dll SHA-256"]
    C --> D{settings.json\npluginEnabled?}
    D -- 否 --> D0["不启动 scanner / observer / audio hook\nUI 控制回调置空\n状态=disabled_by_user"]
    D -- 是 --> E["disableAllEffectsAtStartup()\n所有 global/track 当前会话先 bypass"]
    E --> F["gp_audio::initialize()\n安装 Qt event filter / 可选 Qt object registry hook"]
    F --> G["hook::prepare(host)"]
    G --> H{host hash 通过?}
    H -- 否 --> H0["不读私有 ABI\n不 patch\n保持旁路\nreason=host_unsupported"]
    H -- 是 --> I["查找 GPRSE/AMAudio 导出和 stream RVA"]
    I --> J{export/accessor\n存在?}
    J -- 否 --> J0["不安装或回滚 patch\nreason=entry_points_not_found / buffer_accessors_not_found"]
    J -- 是 --> K["memcmp prologue\nMaster / DSP / stream"]
    K --> L{prologue 匹配?}
    L -- 否 --> L0["拒绝对应 patch\n保留旁路"]
    L -- 是 --> M["VirtualAlloc trampoline\n写 jump + FlushInstructionCache"]
    M --> N["安装 master + dsp\nstream 为可选输出观测/输入介入"]
    N --> O{GPVST3_ENABLE_P2_HOOK=0?}
    O -- 是 --> O0["保持实时接入关闭"]
    O -- 否 --> P["发布 hook::installed\n配置 input router\n所有 chain 默认 bypass"]
    P --> Q{GPVST3_ENABLE_P2_EFFECT=1?}
    Q -- 是 --> Q0["可选 legacy RuntimeEffect\nconfigureRuntimeChain + reconfiguration probe"]
    Q -- 否 --> Q1["不创建默认 runtime effect\n等待用户显式选择"]
```

门控有三层：

1. **插件启动开关**：`settings.json` 关闭时不启动扫描、观测和音频 hook。
2. **宿主版本门控**：五个宿主文件的 SHA-256 必须全部匹配；否则私有 ABI 不触碰。
3. **函数级门控**：导出地址、stream RVA、函数 prologue 必须满足当前版本；失败会回滚已经写入的 patch。

## 4. 插件发现、缓存与后台识别

```mermaid
flowchart TD
    S["beginAsync(hostSupported, retryTimedOut)"] --> S0{已有 scanFuture 或 recognition job?}
    S0 -- 是 --> S1["返回当前 snapshot"]
    S0 -- 否 --> R["读取根目录\nGPVST3_VST3_PATHS / GPVST3_VST3_ROOT\n否则 Program Files/Common Files/VST3"]
    R --> C["读取 vst3-catalog-cache.json\n校验 schema/scanner/x64/scope"]
    C --> D["后台 static scan"]
    D --> D1["递归发现 .vst3\n拒绝网络路径和 symlink"]
    D1 --> D2["fingerprint\n相对路径 + size + mtime + moduleinfo bytes"]
    D2 --> D3["架构检查\nPE MZ/PE，必须 x64"]
    D3 --> D4{缓存 fingerprint\n和 entries 有效?}
    D4 -- 是 --> D5["cacheReused++\n直接使用 entries"]
    D4 -- 否 --> D6["读取 moduleinfo.json\n解析 Fx Audio Module Class\n排除 Instrument"]
    D6 --> D7{moduleinfo 可识别?}
    D7 -- 是 --> READY["catalog entry\nidentified=true\nrecognition_status=ready"]
    D7 -- 否 --> QUEUED["catalog entry\nclass_id 为空\nrecognition_status=queued"]
    D5 --> W["写回缓存 + publish snapshot"]
    READY --> W
    QUEUED --> W
    W --> P["poll() 发现 scan future 完成"]
    P --> Q["读取 sidecar\n优先排队已启用模块"]
    Q --> Q1["recognitionQueue\n只允许一个 detached worker"]
    Q1 --> Q2["recognitionControl = identifyBundle"]
    Q2 --> Q3["host verify\nLoadLibrary / InitDll / GetFactory\n枚举 Audio Module Class"]
    Q3 --> Q4{10 秒内返回?}
    Q4 -- 是且有识别结果 --> Q5["persistRecognition\nstatus=ready\nsource=factory"]
    Q4 -- 是但失败 --> Q6["status=failed\nretry_after=+60s"]
    Q4 -- 否 --> Q7["status=timeout\n从当前 UI catalog 移除\n迟到结果丢弃"]
    Q5 --> UI["setVst3Catalog + scanFeedback\n只有 ready/有 class_id 条目可操作"]
    Q6 --> UI
    Q7 --> UI
```

```mermaid
stateDiagram-v2
    [*] --> missing
    missing: 无缓存或新 bundle
    missing --> static_scanning: beginAsync
    static_scanning --> ready: moduleinfo.json 识别 Fx
    static_scanning --> queued: metadata_missing/unsupported
    queued --> running: poll 启动 recognition worker
    running --> ready: factory 识别成功
    running --> failed: factory 返回失败
    running --> timeout: 超过 10 秒
    failed --> queued: retry_after 到期或手动刷新
    timeout --> queued: 手动刷新 retryTimedOut=true
    ready --> cached: 写入 catalog cache
    cached --> static_scanning: 指纹变化/扫描器版本变化
    failed --> cached
    timeout --> cached
```

静态扫描只读文件；真正的第三方 `LoadLibrary`、factory 和生命周期识别只发生在显式识别或用户选择准备路径。识别超时没有第三方取消 ABI，worker 会脱离队列，迟到结果不会重新进入当前 catalog。

## 5. UI 选择、持久化和 selection worker

```mermaid
sequenceDiagram
    participant User as 用户
    participant Panel as P7Panel(global/track)
    participant Sidecar as effect-chain.json
    participant Hook as gp_hook API
    participant Worker as selectionWorkerLoop
    participant Chain as 双槽 Chain
    participant Input as live-input copy
    participant Tick as 合并控制通知

    User->>Panel: 勾选/取消勾选、排序、参数编辑
    Panel->>Panel: 更新 enabled/bypass/order/configured
    Panel->>Panel: 关闭被停用插件的 editor
    Panel->>Hook: publishSelection(selection + component/controller state)
    alt global
        Hook->>Hook: requestGlobalVst3Selection()
    else track
        Hook->>Hook: requestTrackVst3Selection(trackKey, selection)
    end
    Hook->>Hook: 记录 request_id、generation、queued_at
    alt selection 为空
        Hook->>Chain: 立即 setBypassed(true)
        Hook->>Input: 立即 setBypassed(true)
    end
    Hook-->>Panel: accepted=true，UI 显示“请求中”
    Panel->>Sidecar: saveRuntimeState(false)
    Worker->>Worker: coalesce 同 scope 的新请求
    Worker->>Hook: 检查 generation 是否过期
    alt 过期
        Worker-->>Hook: 丢弃旧结果，不回写 UI
    else 当前 generation
        Worker->>Hook: 必要时 prepare(host)
        Worker->>Chain: prepareSlot(非活动槽)
        Chain->>Chain: reader drain + 复用相同 identity/state/rate
        Chain->>Hook: RuntimeEffect initialize / restore / setup
        Worker->>Chain: activate(target) 或直接旁路空选择
        Worker->>Input: configureInputSelection(selection)（global 后顺序执行）
        Worker->>Hook: appliedSelection / audioGeneration++ / status=applied
    end
    Worker->>Tick: g_selectionStateChanged=true
    Tick->>Panel: reloadVst3Selections + syncVst3Selection
```

### 选择状态机

```mermaid
flowchart LR
    IDLE["idle"] --> QUEUED["queued\nrequest_id/generation"]
    QUEUED --> PREP["preparing\nworker busy"]
    PREP --> APPLIED["applied\nprepared_at/committed_at\naudioGeneration"]
    PREP --> FAILED["failed\n保留 appliedSelection\n失败项写 sidecar last_error"]
    QUEUED --> QUEUED2["新请求覆盖 pending\n旧 generation 丢弃"]
    APPLIED --> QUEUED
    FAILED --> QUEUED
    APPLIED --> BYPASS["空选择或取消勾选\n下一 callback 旁路"]
    BYPASS --> QUEUED
```

P4 兼容路由中，global 成功后先 `configureSelectedChain`，再 `configureInputSelection`；两条链使用同一 worker 但不是单个原子提交。若 global 成功而 legacy input 准备失败，状态字段和各链旁路结果必须以 `p2-observation.json` 为准。P13 独立 input 请求和配置不走该关联。

### UI 挂载和用户动作

```mermaid
flowchart LR
    MAIN["GP 主窗口"] --> RACK["soundRack"]
    MAIN --> MASTER_UI["soundMastering"]
    RACK --> TRACK_SECTION["gpvst3TrackVst3Section\n当前音轨 VST3"]
    MASTER_UI --> GLOBAL_SECTION["gpvst3GlobalVst3Section\n全局 Master VST3"]
    TRACK_SECTION --> TRACK_LIST["active / available list\ntrackKey 作用域"]
    GLOBAL_SECTION --> GLOBAL_LIST["active / available list\nglobal 作用域"]
    TOOLBAR["标题工具栏"] --> ABOUT["About\n启动时启用插件开关"]
    BUTTON["VST3 扫描按钮"] --> SCAN_ACTION["refresh catalog / retry timeout"]
    TRACK_LIST --> CHECK["复选框\nenabled/bypass/order"]
    GLOBAL_LIST --> CHECK
    TRACK_LIST --> SORT["拖动 / Alt+Up / Alt+Down"]
    GLOBAL_LIST --> SORT
    TRACK_LIST --> EDITOR["双击名称 / 上下文菜单\n打开原生 GUI"]
    GLOBAL_LIST --> EDITOR
    CHECK --> REQUEST["publishSelection\nrequest global/track selection"]
    SORT --> REQUEST
    REQUEST --> WORKER["selection worker"]
    EDITOR --> EDITOR_PATH["openVst3Editor / openTrackVst3Editor"]
    SCAN_ACTION --> CATALOG["vst3_catalog"]
    ABOUT --> SETTINGS["settings.json"]
```

Track 区插入在 GP 的 `soundRack` 后，global 区插入在 `soundMastering` 后；两区独立读取 `effect-chain.json` 的 scope。扫描中的、识别失败的、超时的和缺少 `class_id` 的 bundle 可保留在缓存或 sidecar 中，但不会生成可操作的 UI 行。

## 6. VST3 实例准备与参数通路

```mermaid
flowchart TD
    E["SelectionSlot::prepare(entries, rate, 16384, previous)"] --> E1{条目数 <= 8?}
    E1 -- 否 --> FAIL0["runtime_vst3_chain_full"]
    E1 -- 是 --> E2["准备两块 PlanarBuffer pipeline"]
    E2 --> E3{previous 中 identity + component/controller state + rate 完全匹配?}
    E3 -- 是 --> REUSE["复用 warm RuntimeEffect"]
    E3 -- 否 --> NEW["RuntimeEffect::initialize"]
    NEW --> L["LoadLibraryW runtime .vst3\nInitDll / GetPluginFactory"]
    L --> C["Qt 主线程：create component\nsetHostContext / initialize / activate bus\ngetState + setState"]
    C --> P["获取 IAudioProcessor"]
    P --> CTRL["Qt 主线程：创建独立 controller\n或从 component 获取 controller"]
    CTRL --> H["IComponentHandler\nconnection points\nsetComponentState"]
    H --> R["恢复 saved component/controller state"]
    R --> S["setupProcessing(kRealtime, kSample32)\nsetActive(true) / setProcessing(true)"]
    S --> B["scratch.prepare(2, 16384)\nready=true"]
    REUSE --> SLOT["count=entries.size()\n发布到非活动 slot"]
    B --> SLOT
    SLOT --> A["Chain::activate(target)\n切换 activeSlot + ramp"]
    A --> RUN["实时 processBlock"]
    NEW -.-> FAIL["任一阶段异常/失败\nslot shutdown，失败回退/旁路"]

    RUN --> PC["parameterChanges.drain()"]
    PC --> PROC["audio::process -> VST3 ProcessData"]
    PROC --> OUT["copyFromPlanar\n写回 GP buffer"]

    EDIT["IComponentHandler::performEdit"] --> Q["RuntimeEffect::queueParameter"]
    Q --> PUB["无锁参数队列 publish"]
    Q --> MIRROR["按 module/class 镜像到 live-input copy"]
    PUB --> PC
```

`RuntimeEffect` 的 component/controller 创建、初始化、状态恢复和 editor 相关调用会回到 Qt 主线程；处理器的 `setupProcessing`、scratch 准备和链交接在控制 worker 上完成。实时回调只执行预分配缓冲区和已发布 processor。

## 7. global / Master 实时路径

```mermaid
flowchart TD
    M0["GP 调用 Master::process(self, AudioBuffer, …)"] --> M1["masterProcessHook 进入\nsequence++ / 记录线程、buffer、frames、channels、rate"]
    M1 --> M2["可选 before hash（只为首次写回证据）"]
    M2 --> M3["调用原 Master::process\nGP 原生音源、原生 Master 效果完成"]
    M3 --> M4["updateAudioLayerState\ninputLevel / isRunning / bufferSize"]
    M4 --> M5["rawData -> BlockView\ninputs=outputs=同一 GP AudioBuffer"]
    M5 --> M6{active slot\n且 rate/frame 配置匹配?}
    M6 -- 否 --> MB["configurationMismatchBlocks++\naudio::bypass(block)"]
    M6 -- 是 --> M7["global effects::Chain::process"]
    M7 --> M8{bypass / error / incomplete?}
    M8 -- 是 --> MB
    M8 -- 否 --> M9["SelectionSlot 串联每个 RuntimeEffect\n每个效果器写入下一个 scratch/最终 buffer"]
    M9 --> M10["成功计数 + ramp + 首个处理块证据"]
    MB --> M11["after hash / buffer write evidence"]
    M10 --> M11
    M11 --> M12["返回 GP 原 Master 调用者"]
```

### global 顺序

```mermaid
flowchart LR
    G0["GP 原生音源/原生 Master"] --> G1["Master::process 原函数"]
    G1 --> G2["global VST3 chain\n按 UI 顺序从上到下"]
    G2 --> G3["共享/继续使用同一 AudioBuffer"]
    G3 --> G4["后续 GP output callback"]
```

## 8. track / 音轨实时路径

```mermaid
flowchart TD
    T0["gp_audio::refresh()\n发现 document / track / EffectsChain"] --> T1["reconcileTrackIdentities()\ndocumentId + trackId -> persistent trackKey"]
    T1 --> T2["refreshTrackContextImpl()\n构建最多 64 个 self -> TrackRuntime dispatch"]
    T2 --> T3["TrackDispatchUpdate\ngeneration 置奇数，清空表，等待 readers"]
    T3 --> T4["每个 active document binding\n分配/复用最多 32 个 TrackRuntime"]
    T4 --> T5["runtime.prepare(selection, rate, 16384)\n独立双槽 SelectionSlot"]
    T5 --> T6["发布 dispatch.runtime + dispatch.self\ngeneration 恢复偶数"]

    D0["GP 调用 EffectsChain::processDSP(self, buffer, …)"] --> D1["dspProcessHook\nsequence++ / 记录 self、buffer、index"]
    D1 --> D2["observeEffectsChainContext(self)\n仅诊断 index，真正 scope 来自 dispatch 表"]
    D2 --> D3["调用原 processDSP\nGP 原生音轨链完成"]
    D3 --> D4["TrackDispatchRead\n读取稳定 dispatch"]
    D4 --> D5{找到 self 且上下文稳定?}
    D5 -- 否 --> TB["trackScopeUnresolved=true\n直接返回，保持旁路"]
    D5 -- 是 --> D6["校验 AudioBuffer\nframes <= 16384，channels 1/2，rawData 有效"]
    D6 --> D7{校验通过?}
    D7 -- 否 --> TB
    D7 -- 是 --> D8["TrackRuntime::processBlock"]
    D8 --> D9["track Chain.process\n写回对应音轨 AudioBuffer"]
    D9 --> D10["记录 processed/writeObserved/error/bypass"]
```

track runtime 只在 `self` 与已发布 binding 匹配时处理。切换音轨、增删音轨、保存/另存、重开或 host 对象重建都会进入 control tick，先保存旧 runtime，再以新的 `trackKey` 重建 dispatch；未解析的上下文安全旁路。

## 9. P4 兼容 live-input / PortAudio 介入路径

```mermaid
flowchart TD
    I0["AMAudio PortAudio callback(input, output, frames, …)"] --> I1["streamCallbackHook\n记录 output 地址/调用次数/前 hash"]
    I1 --> I2["调用原 PortAudio callback\nGP 先完成设备回调和 generated output"]
    I2 --> I3["读取 portaudio::Configuration\ninput/output channels、sampleRate、device"]
    I3 --> I4{配置有效且\nRouter enabled + route != disabled?}
    I4 -- 否 --> I5["保持原 output\n记录 output 观测"]
    I4 -- 是 --> I6["构造 InterleavedView\n借用 input/output 指针，仅限本 callback"]
    I6 --> I7["processExternalInputInterleaved"]
    I7 --> I8{Float32 / channels / frames / rate 匹配?}
    I8 -- 否 --> I9["configurationErrors / missingBlocks\n返回 false，保留 GP output"]
    I8 -- 是 --> I10["deinterleave input -> planar scratch"]
    I10 --> I11{Router active?\nenabled + !bypassed + streamRunning}
    I11 -- 否 --> I12["passthrough\n无处理旁路"]
    I11 -- 是 --> I13{route}
    I13 -->|input_insert| II["capture -> VST3 input chain\n结果写入 output scratch"]
    I13 -->|bus_mix| IM["capture + 原 output generated\n逐通道相加 -> VST3 input chain"]
    II --> I14["finiteOutput 校验"]
    IM --> I14
    I14 --> I15{处理成功?}
    I15 -- 否 --> I16["errorBlocks++\naudio::bypass(block)"]
    I15 -- 是 --> I17["interleave planar -> borrowed output\n单声道输出时双声道平均"]
    I12 --> I17
    I16 --> I17
    I17 --> I18["记录 postOriginal/postRoute hash\n记录 capture/generated/output 首样本"]
    I18 --> I19["最终 output hash 改变证据\n返回原 callback result"]
    I5 --> I19
    I9 --> I19
```

### 两条输入路由的差异

| 路由 | 处理输入 | 处理输出 | 失败行为 | 当前计数 |
| --- | --- | --- | --- | --- |
| `disabled` | 不处理 | 保留 GP 原 output | 不进入 VST3 | `input_bypass_blocks` |
| `input_insert` | 仅 capture | capture 经 input chain 直接写 output | 复制 capture 旁路 | `input_processed_blocks` |
| `bus_mix` | capture + GP 原 output/generated | 混合后经 input chain 写 output | 混合输入旁路 | `input_bus_mixed_blocks` |

默认安装路径不启用 live-input 监听；显式设置 `GPVST3_P4_ROUTE=input_insert|bus_mix` 后才会把选中的 global chain 复制为独立 live-input chain，可改用独立 `GPVST3_RUNTIME_VST3`。

## 10. 双槽切换、实时安全和故障回退

```mermaid
stateDiagram-v2
    [*] --> Bypassed
    Bypassed: requestedBypass=true 或无 active slot
    Bypassed --> Preparing: control worker prepareSlot
    Preparing: 目标槽 accepting=false\nreader drain\n实例初始化/复用
    Preparing --> Ready: configured=true
    Preparing --> Fault: 初始化失败
    Ready --> Active: activate(target)\nactiveSlot 原子发布
    Active: audio callback acquire active slot\nreader++
    Active --> Active: process block 成功\nramp / firstProcessed evidence
    Active --> Bypassed: requested bypass / 空选择
    Active --> Fault: processor false / exception / invalid output
    Fault: faulted=true\nbypassed=true\nfallbackBlocks++
    Fault --> Preparing: 下一次有效请求重建
    Active --> Switching: prepare inactive slot
    Switching: 旧 slot accepting=false\n新 slot ready 后 activate
    Switching --> Active: reader drain 完成\n新 generation 生效
    Bypassed --> Active: warm slot 重新 activate
```

`effects::Chain::process` 的实时分支只有有限两次 acquire 尝试；拿不到稳定 active slot 时直接旁路。插件处理失败会将 chain 标记为 faulted 并旁路当前块，控制路径随后可隔离失败效果器、保存剩余项并重建链。切换的 reader drain 在控制路径执行，实时线程不等待锁。

## 11. 原生 editor 生命周期

```mermaid
sequenceDiagram
    participant User as 用户/宿主桥接
    participant Panel as P7Panel
    participant Qt as Qt 主线程
    participant Hook as openVst3Editor/openTrackVst3Editor
    participant Effect as RuntimeEffect
    participant Editor as editor worker + IPlugView
    participant VST as 第三方 controller/view

    User->>Panel: 双击名称 / 上下文菜单 / host bridge
    Panel->>Panel: 要求 enabled；若 busy 保存 pending editor key
    Panel->>Qt: 创建/复用 NativeEditorWindow\nWA_NativeWindow，取得 HWND
    Panel->>Hook: 按 scope + module + class_id 找 active instance
    Hook->>Effect: openEditor(HWND)
    Effect->>Qt: 要求当前调用在 Qt 线程
    Effect->>Editor: 创建独立线程，COM apartment
    Editor->>VST: createView("editor")
    VST-->>Editor: IPlugView 或 null
    Editor->>VST: isPlatformTypeSupported(HWND)
    Editor->>VST: setFrame(RuntimePlugFrame)
    Editor->>VST: setContentScaleFactor（如支持）
    Editor->>VST: getSize()
    Editor->>Qt: invoke resizeNativeEditor(HWND, width, height)
    Editor->>VST: attached(HWND, kPlatformTypeHWND)
    Editor->>VST: onSize()
    Editor-->>Effect: editorThreadReady / editorAttached
    Effect-->>Hook: stage=visible，保存 g_openEditorEffect
    Hook-->>Panel: 显示并激活非 modal 窗口
    User->>Panel: 关闭窗口 / 切换到另一个 editor
    Panel->>Hook: closeVst3Editors()
    Hook->>Editor: editorThreadStop=true
    Editor->>VST: removed() -> setFrame(nullptr)
    Editor->>Qt: 嵌套事件循环等待销毁完成
    Hook-->>Panel: stage=removed
    Note over Hook,VST: 关闭 editor 只移除 view，不停用音频实例
```

失败阶段写入 `editor_stage`、`editor_result_code`、`editor_error`，包括 `controller_missing`、`create_view`、`platform_check`、`set_frame`、`get_size`、`attached` 和 `removed`。UI 只显示中性文案；识别中的插件会等 control tick 发现 worker 空闲后重试一次 pending key。

## 12. 音轨上下文、文档身份与持久化

```mermaid
flowchart LR
    DOC["GP document / score"] --> DISC["gp_audio collect()\nQt object registry / MCP bridge"]
    DISC --> ID["documentId + scoreKey + trackId + index"]
    ID --> REC["state::reconcileTrackIdentities"]
    REC --> KEY["persistent runtimeKey\ndocumentId#track-UUID"]
    KEY --> RT["TrackRuntime table\n最多 32 个 runtime / 64 个 dispatch"]
    KEY --> SIDE["effect-chain.json\nscores.score.tracks.key"]

    GLOBAL["global selection"] --> SIDE2["effect-chain.json\nglobal.effects"]
    UI["P7Panel 保存/关闭/参数编辑"] --> CAP["captureState()\ncomponent_state + controller_state"]
    CAP --> SIDE
    CAP --> SIDE2
    SIDE --> START["下一次启动"]
    SIDE2 --> START
    START --> OFF["disableAllEffectsAtStartup\nenabled=false, bypass=true, desired_enabled=true"]
    OFF --> LIST["UI 仍显示保存的链和 state"]
    LIST --> ENABLE["用户再次显式勾选"]
    ENABLE --> RT
    ENABLE --> GLOBAL
```

状态文件职责：

| 文件 | 写入时机 | 内容 |
| --- | --- | --- |
| `settings.json` | About 开关 | `enabled`，决定下次启动是否扫描和接入 |
| `effect-chain.json` | 勾选、排序、参数、关闭、退出、runtime retire | schema 2 的 global/track 链、顺序、enabled/bypass、component/controller state、错误 |
| `vst3-catalog-cache.json` | static scan、factory recognition、timeout | 路径指纹、metadata entries、recognition 状态和重试时间 |
| `status.json` | bootstrap 初始化和 scan poll | host、VST3 catalog、hook、UI、adapter 摘要 |
| `p2-observation.json` | 后台合并快照和退出最终 flush | Master/DSP/stream、chain、track、input、selection、editor 时间线和证据 |

## 13. 控制线程、实时线程和 editor 线程关系

```mermaid
flowchart TB
    subgraph CTRL["控制域：可分配、可等待、可访问 Qt/文件"]
        QT["Qt 主线程\nUI / timers / state writes"]
        SC["scan worker\nstatic scan future"]
        REC["recognition worker\n一次一个，10s timeout"]
        SEL["selection worker\n实例准备 / chain switch / track prepare"]
        REG["gp_audio registry refresh\ntrack dispatch publish"]
    end
    subgraph AUDIO["实时域：不分配、不扫描、不访问 Qt"]
        MASTER2["masterProcessHook"]
        DSP2["dspProcessHook"]
        STREAM2["streamCallbackHook"]
        CHAIN2["Chain / RuntimeEffect::processBlock"]
        ROUTER2["Router::processInterleaved"]
    end
    subgraph EDIT["editor 域：第三方 view contract"]
        EW["persistent editor thread\nCOM apartment"]
        HWND["NativeEditorWindow HWND"]
    end

    QT --> SEL
    QT --> SC
    QT --> REG
    SC --> REC
    SEL --> CHAIN2
    REG --> DSP2
    QT --> EW
    EW <--> HWND
    EW --> QT
    MASTER2 --> CHAIN2
    DSP2 --> CHAIN2
    STREAM2 --> ROUTER2
    ROUTER2 --> CHAIN2
    CHAIN2 -.-> QT
    MASTER2 -.-> QT
    DSP2 -.-> QT
    STREAM2 -.-> QT
```

实时线程读取的对象都是控制线程已经发布的固定对象或原子字段：

- global/track/input 各自的 active slot、processor callback 和预分配 scratch；
- `selectionPublished`、`selectionConfiguredRate`、`audioGeneration`、bypass/fault 标志；
- track `self -> TrackRuntime` dispatch 表和 generation/readers 保护；
- input Router 的 route、enabled、streamRunning、配置参数；
- 参数队列中的已发布值。

## 14. 失败、旁路和宿主限制

| 触发条件 | 实时行为 | 控制/诊断行为 |
| --- | --- | --- |
| 宿主 SHA-256 不匹配 | 不安装私有 hook，保持旁路 | `host_unsupported` |
| hook 导出/prologue 不匹配 | 拒绝或回滚 patch | `entry_points_not_found` / `hook_install_failed` |
| 未解析音轨 `self` | 不执行 track runtime | `track_scope_unresolved=true` |
| frames > 16384、channels 非 1/2、rate 不匹配 | 当前块旁路或丢弃 | `configurationMismatchBlocks` / input configuration errors |
| VST3 初始化、state restore、setupProcessing 失败 | 新 slot 不发布，旧链保留；无旧链则旁路 | 失败项写 `last_error`，selection=`failed` |
| VST3 `process` 返回失败或抛异常 | 当前 chain 标记 faulted，当前块 fallback bypass | `errorBlocks`、`fallbackBlocks`，后续可隔离失败项 |
| 输出含非 finite 值 | input 路由回退旁路 | `errorBlocks` |
| reader drain / selection 忙 | 实时线程最多两次 acquire，拿不到就旁路 | 控制 worker 继续处理，UI 保持可操作 |
| editor 无 controller/view/HWND 或 contract 失败 | 音频实例继续运行，editor 不显示 | `editor_stage/result_code/error` |
| 识别超过 10 秒 | 当前 catalog 隐藏该 bundle，队列继续 | detached worker 迟到结果丢弃，缓存标记 timeout |
| 第三方插件进程内崩溃 | 当前实现没有进程级隔离 | 仍属于未实现的隔离能力 |

## 15. 当前实现与证据边界

- 已验证目标：Guitar Pro 8.1.1.17、Windows x64；global/track/live-input 的代码路径、双槽切换、原生 editor 生命周期、状态恢复和识别超时均有专项或 MCP 证据。
- 真实设备矩阵（ASIO/WASAPI）、真实扬声器听感、capture 监听/反馈和其他 Guitar Pro 版本仍受宿主条件限制。
- `status.json` / `p2-observation.json` 中的 `selection_*`、`audio_generation`、`chain_*first_processed*`、`input_*`、`editor_stage/editor_result_code` 是判断“请求已接受、准备完成、已提交、首个有效处理块、输出已改变、editor 可见”的证据入口。
- “DLL 加载成功”“菜单可枚举”“请求返回 true”只表示流程进入了某个阶段，不能单独证明实时 buffer 已写回或真实听感已生效。

## 16. 代码入口索引

| 逻辑 | 文件/符号 |
| --- | --- |
| 自动加载和退出 | `native/vst3_autoload.cpp`：`GuitarProVst3Autoload`、`initializePlugin`、`stopObservation` |
| 启动编排和状态 JSON | `native/modules/bootstrap.cpp`：`initialize`、`pollVst3`、`hookSnapshot` |
| 宿主门控和三个 hook | `native/modules/gp_hook.cpp`：`prepare`、`masterProcessHook`、`dspProcessHook`、`streamCallbackHook` |
| 选择 worker | `native/modules/gp_hook.cpp`：`request*Selection`、`selectionWorkerLoop` |
| 双槽交接 | `native/modules/effect_chain.cpp`：`prepareSlot`、`activate`、`process` |
| VST3 生命周期 | `native/modules/gp_hook.cpp`：`RuntimeEffect::initialize`、`reconfigure`、`processBlock` |
| 输入转换和路由 | `native/modules/input_router.cpp`：`process`、`processInterleaved` |
| track 绑定 | `native/modules/gp_audio_runtime.cpp`：`initialize`、`refresh`、`currentTrack`、`lookup` |
| 扫描与识别 | `native/modules/vst3_catalog.cpp`：`beginAsync`、`poll`、`startNextRecognition`；`vst3_host.cpp`：`identifyBundle` |
| UI 选择/editor | `native/modules/qt_ui.cpp`：`P7Panel::publishSelection`、`openEditor` |
| 持久化和身份 | `native/modules/state_manager.cpp`：`disableAllEffectsAtStartup`、`reconcileTrackIdentities`、`writeChain` |
