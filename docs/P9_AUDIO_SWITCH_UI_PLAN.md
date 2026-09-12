# P9：实时切换稳定性与界面体验计划

状态：已完成；editor 与首次启停反馈已由 P10 收敛（2026-09-13）
基线：`v0.9.0` / Guitar Pro 8.1.1.17 / Windows x64。

本计划的切换框架、UI 夹具和 MCP 流程已完成，但真实使用中仍发现 VST3 原生 editor 打不开，以及首次启停后实际声音效果延迟数秒。新的定位、修复和验收要求见 [P10 优化计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)。原有夹具证据继续保留，不能替代 P10 的真实宿主验收。

## 完成状态

| 编号 | 状态 | 证据 |
| --- | --- | --- |
| P9-1 | 框架已实现；真实首次启停延迟待 P10 | `effect_chain` 双槽交接、异步 selection worker、有限 reader drain 观测、128-sample ramp；`test-p9-switch.ps1` 通过，但未证明真实插件首个声音块的生效时延。 |
| P9-2 | 已实现、已验证 | 紧凑 global/track 行、稳定 objectName、260/320/420 px 与 100/125/150% DPI 夹具通过。 |
| P9-3 | 已实现、已验证/宿主受限回退 | 标题工具栏只保留一个 About 入口并清理旧的 VST3 fallback；详情窗口提供打开配置和启动开关。 |
| P9-4 | 已实现、已验证 | 用户界面仅显示中性扫描状态，详细错误保留在结构化状态/日志；UI 夹具通过。 |
| P9-5 | UI 入口已实现；真实第三方 editor 打开待 P10 | 插件名称双击和上下文 action 共用 editor 路径；fixture/MCP 窗口流程通过，但真实 VST3 controller/editor 合同和失败阶段尚未闭环。 |
| P9-6 | 已实现、已验证 | sidecar 只保存显式状态并自动压缩旧的空拓扑记录，避免缓存持续增长。 |

## 已交付实现

- `native/modules/effect_chain.*`：切换计数、准备/交接/reader drain/ramp/序列连续性观测。
- `native/modules/gp_hook.*`：global/track 请求异步合并，准备完成后原子交接，失败回退与直接旁路。
- `native/modules/qt_ui.*`：紧凑链界面、请求状态反馈、About 入口和中性扫描反馈。
- 新增 `native/test/p9_switch_test.cpp`、`p9_ui_test.cpp` 及对应 PowerShell 入口。

## 验证与边界

已执行并通过：

- `./native/test/test-p9.ps1`
- `./native/test/test-p8-runtime.ps1`
- `./native/test/test-p8-ui.ps1`
- `./native/test/test-p7-ui.ps1`
- `./native/test/test-p8-ui.ps1`
- MCP：`test-p7-mcp.ps1`，使用本次构建的 autoload DLL 与 Gateway/test VST3。
- `./native/build.ps1 -OutputRoot .tools/native/p9-build`

证据目录：`.tools/native/p9-delivery-suite`、`artifacts/mcp-p7-*`、`artifacts/p8-runtime-*`。

真实宿主中已验证流程启动、扫描、global/track 切换和恢复；插件 editor 的真实第三方兼容性、首次启停首个声音块延迟、真实扬声器听感阈值、ASIO/WASAPI 设备矩阵及第三方插件崩溃隔离待 P10 或后续宿主受限验证。
