# P9 实现记录

状态：已完成；P10 已补齐真实宿主反馈项（2026-09-13）
关联计划：[P9：实时切换稳定性与界面体验计划](P9_AUDIO_SWITCH_UI_PLAN.md)；后续：[P10 editor/音频生效优化计划](P10_EDITOR_AUDIO_ACTIVATION_PLAN.md)
基线：`v0.9.1` / Guitar Pro 8.1.1.17 / Windows x64。

## 用户反馈与实现状态

| 编号 | 状态 | 实现与证据 |
| --- | --- | --- |
| P9-1 | 框架已实现；真实首次启停延迟待 P10 | 异步选择 worker、双槽准备后交接、失败保留旧链/旁路、ramp 与 reader drain 指标；现有专项通过，但未覆盖真实插件首个声音块延迟。 |
| P9-2 | 已实现、已验证 | global/track 紧凑行、空/扫描状态、稳定 objectName；`test-p9-ui.ps1` 通过。 |
| P9-3 | 已实现、已验证/宿主受限回退 | 标题工具栏只保留一个 About 入口；旧的 VST3 菜单 fallback 会被清理。详情窗口提供配置路径和启动开关。 |
| P9-4 | 已实现、已验证 | 错误详情留在 `status.json`/日志，按钮、状态和 tooltip 使用中性文案；UI 夹具通过。 |
| P9-5 | UI 入口已实现；真实第三方 editor 打开待 P10 | 插件名称双击打开 GUI，移除每行独立 GUI 按钮；fixture/MCP 流程通过，但真实第三方 controller/editor 打开失败仍待定位。 |
| P9-6 | 已实现、已验证 | 状态层只保存显式配置和插件状态，空拓扑不落盘；读取旧的大 sidecar 时自动一次性压缩。 |

## 验证记录

已执行并通过：

- `./native/build.ps1 -OutputRoot .tools/native/p9-build`
- `./native/test/test-p9.ps1`（switch + UI，证据：`.tools/native/p9-delivery-suite`）
- `./native/test/test-p8-runtime.ps1`（证据：`artifacts/p8-runtime-*`）
- `./native/test/test-p8-ui.ps1`
- `./native/test/test-p7-ui.ps1`
- MCP 宿主：`./native/test/test-p7-mcp.ps1`，使用本次构建 DLL、测试 Gain Fixture 与 Gateway，流程通过。
- `./native/test/test-p5-state.ps1`、`test-p8-state.ps1`、`test-p7-ui.ps1`、`test-p8-ui.ps1`、`test-p9-ui.ps1`。

## 代码入口

- `native/modules/effect_chain.*`：槽交接、ramp、序列和 drain 观测。
- `native/modules/gp_hook.*`：异步 global/track 选择和失败回退。
- `native/modules/qt_ui.*`：P9 面板、About、请求状态和扫描反馈。
- `native/test/p9_switch_test.cpp`、`p9_ui_test.cpp`：可重复夹具。

## 证据边界

真实宿主流程覆盖启动、扫描、global/track 切换和恢复；真实第三方 editor 兼容性、首次启停首个声音块延迟、真实扬声器听感阈值、ASIO/WASAPI 完整设备矩阵和第三方插件进程级隔离待 P10 或后续宿主受限验证。
