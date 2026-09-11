# P9：实时切换稳定性与界面体验计划

状态：已完成（2026-09-12）  
基线：`v0.8.0` / Guitar Pro 8.1.1.17 / Windows x64。

本计划的四项问题均已实现并完成夹具及 MCP 宿主验证；真实扬声器听感和完整设备矩阵仍按证据边界记录。

## 完成状态

| 编号 | 状态 | 证据 |
| --- | --- | --- |
| P9-1 | 已实现、已验证 | `effect_chain` 双槽交接、异步 selection worker、有限 reader drain 观测、128-sample ramp；`test-p9-switch.ps1` 通过，MCP 宿主流程通过。 |
| P9-2 | 已实现、已验证 | 紧凑 global/track 行、稳定 objectName、260/320/420 px 与 100/125/150% DPI 夹具通过。 |
| P9-3 | 已实现、已验证/宿主受限回退 | 标题工具栏 About 幂等挂载与非模态复用；无稳定 toolbar 时提供菜单 action。 |
| P9-4 | 已实现、已验证 | 用户界面仅显示中性扫描状态，详细错误保留在结构化状态/日志；UI 夹具通过。 |

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

真实宿主中已验证流程启动、扫描、插件/editor、global/track 切换和恢复；未覆盖真实扬声器听感阈值、ASIO/WASAPI 设备矩阵及第三方插件崩溃隔离。
