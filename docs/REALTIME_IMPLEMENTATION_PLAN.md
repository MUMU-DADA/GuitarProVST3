# GuitarProVST3 实时实现总览

当前目标是在 Windows x64 的 Guitar Pro 8.1.1.17 中提供由 Guitar Pro 加载的实时 VST3 效果器链。P8/P9 的实现记录和当前待办以根目录 `docs/` 下的正式文档为准；editor 打开和首次启停生效的后续工作见 [P10 优化计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)，事件驱动切轨与按需运行时见 [P12 计划](P12_EVENT_DRIVEN_LAZY_RUNTIME_PLAN.md)，阶段原始记录保存在 `docs/archive/phase-records/`。

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

## 核心约束

- 未通过 Guitar Pro 文件哈希和函数 prologue 校验时，私有 ABI hook 保持关闭并旁路。
- 音频线程不扫描磁盘、不创建 Qt 对象、不动态分配、不等待阻塞锁；实例和 scratch 在控制线程准备。
- VST3 链独立于 GP 私有 `core::Effect` / `am::overloud::Effect` 容器；global、track 和 P4 input 使用各自明确的处理边界。
- UI 和 sidecar 写入在 Qt/控制线程执行，实时回调只读取已发布的运行时对象。

## 当前宿主边界

- 已验证范围锁定 Guitar Pro 8.1.1.17 / Windows x64；其他版本由哈希门控拒绝实时接入。
- 不同 ASIO/WASAPI 设备、真实扬声器听感、capture 监听/反馈和暂停恢复听感没有完整设备矩阵。
- 第三方插件的进程内崩溃隔离未实现；factory 超时会丢弃迟到结果，但不提供强制终止第三方线程的保证。

用户操作见 [安装与使用](INSTALL.md)，构建和回归见 [测试与验证](TESTING.md)。

完整运行逻辑、实时介入点、线程边界和失败旁路图见[运行逻辑与介入逻辑总图](PLUGIN_RUNTIME_AND_INTERVENTION.md)。
