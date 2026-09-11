# P8 实现记录

P8 六项补齐工作及交付验收已完成（2026-09-11，Guitar Pro 8.1.1.17 / Windows x64）。`EffectsChain self -> track ID` 和可写 `IAudioBuffer` 已用于真实音轨处理；未解析上下文仍安全旁路。重整前的详细记录保留在源码仓库归档中。

## 当前结果

| 阶段 | 已实现并验证 |
| --- | --- |
| P8.7 后台识别 | `queued` / `running` / `failed` / `timeout` 不进入可操作列表；真实 10 秒超时推进队列，缓存重启复用，手动刷新可重试。 |
| P8.8 音轨运行时 | 双轨独立实例、GUI、参数和实际 buffer 写回；增删、重排、撤销、首次保存、Save As、关闭重开及进程重启恢复通过。 |
| P8.9 原生音源链 | 原生音色/效果数组保持不变；真实 GP 原生 EQ 参数、旁路和播放回归通过。 |
| P8.10 分区域 UI | track 内容位于 `soundRack` 后，global 内容位于 `soundMastering` 后；wrapper、布局和分界线断言通过。 |
| P8.11 窄侧栏 | 260/320/420 px 及 100%/125%/150% 缩放下名称、tooltip、复选框和 GUI 控件通过。 |
| P8.12 联调交付 | 核心套件、P4 组合、P7 回归、独立发布运行、版本门控和发布包核对通过。 |

## 实现摘要

- 静态扫描只读取本地文件和缓存；信息不足的 bundle 才由后台 worker 主动识别。识别失败或超时丢弃结果，不强制终止第三方线程。
- schema 2 分离 global 和 track state；运行时 key 组合 document ID 与持久化 track key，避免跨文档共享实例。
- `gp_audio_runtime` 优先使用版本 1 的 GuitarProMCP bridge；没有 bridge 时使用本 DLL 的 native collector。发布 DLL 不依赖 `guitarpro_mcp.dll` 导入库。
- 每轨使用独立双槽链、预分配 scratch、generation/reader drain 和写回标志；global 链在 GP Master 后处理点运行。
- 已启用插件按列表顺序串联；采样率重配置、保存状态和单项故障隔离在控制线程处理。容量上限为 32 个活动音轨、64 个 chain binding、每链 8 个效果器、最多 16384 帧。

## 验证摘要

验证在真实 Guitar Pro 8.1.1.17 的原软件免安装入口完成。完整日志位于本机被忽略的 `artifacts/` 和 `.tools/`，不随发布包提交。

| 验证面 | 实际证据 |
| --- | --- |
| 识别、超时、缓存 | `artifacts/p8-recognition-host-*` |
| MCP bridge 和 native collector 生命周期 | `artifacts/mcp-p8-track-*` |
| 无 MCP bridge 发布运行 | `artifacts/p8-standalone-*` |
| global/track/P4 顺序 | `artifacts/mcp-p8-order-*` |
| 采样率、缺失插件和故障隔离 | `artifacts/p8-runtime-*` |
| 发布包和宿主哈希门控 | `artifacts/p6-package-*`、`.tools/native/p8-delivery-gate/verification.json` |

常用命令见 [测试与验证](TESTING.md)。

## 尚未覆盖

不同 ASIO/WASAPI 设备、真实扬声器听感、capture 监听/反馈稳定性、其他 GP 版本和第三方插件崩溃隔离仍需单独验证。factory 超时采用结果丢弃策略，不提供进程级强制终止保证。
