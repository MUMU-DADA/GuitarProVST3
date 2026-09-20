# GuitarProVST3 实时实现总览

当前目标是在 Windows x64 的 Guitar Pro 8.1.1.17 中提供由 Guitar Pro 加载的实时 VST3 效果器链。P8/P9 的实现记录和当前待办以根目录 `docs/` 下的正式文档为准；editor 打开和首次启停生效的后续工作见 [P10 优化计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)，事件驱动切轨与按需运行时见 [P12 计划](P12_EVENT_DRIVEN_LAZY_RUNTIME_PLAN.md)，ASIO 输入低延迟监听与 RSE 共存见 [P13 计划](P13_LOW_LATENCY_ASIO_INPUT_PLAN.md)，阶段原始记录保存在 `docs/archive/phase-records/`。

## 目标与架构

```text
GuitarPro.exe
  └─ GuitarPro 插件 DLL
       ├─ 内嵌 VST3 Host
       ├─ GP 音频适配与实时接入
       └─ Qt 效果器链界面
```

GP 继续负责音频设备、输入输出、采样率和流生命周期；插件负责 VST3 实例、独立效果器链、中间缓冲和界面。插件不创建独立声卡流、独立服务或独立宿主。

## 阶段状态

| 阶段 | 当前结果 |
| --- | --- |
| P0 | 插件自动加载、宿主版本锁定和默认旁路 |
| P1 | DLL 内嵌 VST3 Host 和实例生命周期 |
| P2 | GP 音频适配、实时块处理和输出回调观测 |
| P3 | 双槽链、原子切换、旁路和错误回退 |
| P4 | capture、`input_insert` 和 `bus_mix` 路由 |
| P5 | Qt 链编辑器和 sidecar 状态 |
| P6 | 哈希门控、回归流程和发布包 |
| P7 | 静态发现、按需识别、缓存和原生 editor |
| P8 | track/global 链、生命周期恢复、UI 分区和交付回归 |
| P9 | 实时切换稳定性、UI 重构、标题工具栏 About 和扫描反馈收敛（夹具/MCP 完成；真实 editor 与首次启停生效问题转入 P10） |
| P10 | VST3 原生 editor 打开、首次启停音频提交、首个有效处理块诊断和真实宿主回归（已完成） |
| P11 | 周期音轨/侧栏维护改为合并通知，诊断后台写盘，scanner 按任务活动 poll（代码与专项完成；长时设备矩阵采样边界见实现记录） |
| P12 | 事件驱动切轨、曲谱生命周期门控、按需实例化和 warm-cache（代码与专项验收完成；宿主受限项见实现记录） |
| P13 | 0.10.1 已实现 LINE-IN 门控、空链干声、输入插件启停/UI 统一、六种采样率和九档 buffer 处理、参数同步与输出范围修复；算法/离线及真实宿主开关、三链、冷启动和编辑器重入回归通过。0.10.2 保留完整优化，用户确认当前演奏场景开启声卡驱动安全模式后爆音消失；完整硬件矩阵仍未完成，见实现记录 |

## 核心约束

- 未通过 Guitar Pro 文件哈希和函数 prologue 校验时，私有 ABI hook 保持关闭并旁路。
- 音频线程不扫描磁盘、不创建 Qt 对象、不动态分配、不等待阻塞锁；实例和 scratch 在控制线程准备。
- VST3 链独立于 GP 私有 `core::Effect` / `am::overloud::Effect` 容器；global、track、P13 input 与 P4 兼容路由使用各自明确的处理边界。低延迟 input 只处理 capture，在 GP callback 返回后叠加，不处理或覆盖 RSE output。
- 输入监听由 Guitar Pro LINE-IN 控制；保存低延迟偏好不打开输入，宿主关闭或状态未知时不处理 capture。空 input 链是正常干声监听，输入插件使用与其他 scope 一致的待提交/实际运行启停流程。
- UI 和 sidecar 写入在 Qt/控制线程执行，实时回调只读取已发布的运行时对象。
- 普通构建使用固定版本 MinHook 和严格的线程检查安装/移除 hook；ASIO stream identity、实际 rate 与 generation 共同门控 input slot。PCM 和长窗口计时仅存在于独立实验构建，不能进入发布包。

## 当前宿主边界

- 已验证范围锁定 Guitar Pro 8.1.1.17 / Windows x64；其他版本由哈希门控拒绝实时接入。
- 0.10.1 已实现实际设备 44100、48000、88200、96000、176400、192000 Hz 的对应 GP 44100 Hz 输出拓扑，buffer 32、64、128、256、512、1024、2048、4096、8192 帧已有离线专项；大 callback 按不超过 2048 帧连续处理。范围外或实际 stream/SRC 合同不符仍报告 `host_limited`，不能用算法通过代替真实驱动打开和监听证据。
- 0.10.0 在 Studio 2 PRO / Midiplus USB Audio ASIO、192000 Hz / 64 帧的物理监听延迟曾由 78.458 ms 降至 3.177 ms，包含监听路径、DAC、线缆和 ADC。0.10.1 已独立复验固定配置的输入开关与三链逻辑；其他 buffer 请求经通用控制接口回滚并恢复 64，8192 未枚举，不能据此推断驱动拒绝。Neural 参数同步优化已有离线输入 DSP 计时改善；用户在 0.10.2 收尾时确认开启声卡驱动安全模式后当前演奏爆音消失，安全模式下的物理延迟未重新测量。
- 第三方插件的进程内崩溃隔离未实现；factory 超时会丢弃迟到结果，但不提供强制终止第三方线程的保证。

用户操作见 [安装与使用](INSTALL.md)，构建和回归见 [测试与验证](TESTING.md)。

完整运行逻辑、实时介入点、线程边界和失败旁路图见[运行逻辑与介入逻辑总图](PLUGIN_RUNTIME_AND_INTERVENTION.md)。
