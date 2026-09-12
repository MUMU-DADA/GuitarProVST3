# GuitarProVST3 实时实现总览

当前目标是在 Windows x64 的 Guitar Pro 8.1.1.17 中提供由 Guitar Pro 加载的实时 VST3 效果器链。当前实现和证据以 [P8 实现记录](P8_IMPLEMENTATION.md) 为准，阶段原始记录保存在 `docs/archive/phase-records/`。

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
| P9 | 实时切换稳定性、UI 重构、标题工具栏 About 和扫描反馈收敛（已完成） |
| P10 | ASIO 输入链、端到端低延迟、曲谱加载性能和音轨身份显示（已完成，见 [P10 实现记录](P10_IMPLEMENTATION.md)） |

## 核心约束

- 未通过 Guitar Pro 文件哈希和函数 prologue 校验时，私有 ABI hook 保持关闭并旁路。
- 音频线程不扫描磁盘、不创建 Qt 对象、不动态分配、不等待阻塞锁；实例和 scratch 在控制线程准备。
- VST3 链独立于 GP 私有 `core::Effect` / `am::overloud::Effect` 容器；global、track 和 P4 input 使用各自明确的处理边界。
- UI 和 sidecar 写入在 Qt/控制线程执行，实时回调只读取已发布的运行时对象。

## 当前宿主边界

- 已验证范围锁定 Guitar Pro 8.1.1.17 / Windows x64；其他版本由哈希门控拒绝实时接入。
- 不同 ASIO/WASAPI 设备、真实扬声器听感、capture 监听/反馈和暂停恢复听感没有完整设备矩阵。
- P10 已补齐 ASIO 输入路由、回调 deadline/延迟结构化诊断、启动时间线和标题栏音轨身份；真实硬件 loopback 的 roundtrip 数值仍按设备可用性单独记录，不能由夹具替代。
- 第三方插件的进程内崩溃隔离未实现；factory 超时会丢弃迟到结果，但不提供强制终止第三方线程的保证。

用户操作见 [安装与使用](INSTALL.md)，构建和回归见 [测试与验证](TESTING.md)。
