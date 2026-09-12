# P11 实现记录

状态：已实现（2026-09-12）

P11 已将启动关键路径与音轨上下文维护解耦：`bootstrap.cpp` 不再在初始化阶段同步遍历音轨，首帧后延迟 1500 ms 才启动维护定时器。VST3 自动扫描由启动 owner 只调用一次 `beginAsync`；周期性 60 秒重扫已移除，用户刷新仍通过 UI discovery control 显式触发，目录和进行中的任务由 `vst3_catalog` 合并。

输入链默认启用（可用 `GPVST3_ENABLE_P4_INPUT=0` 或 `GPVST3_P4_ROUTE=disabled` 关闭），ASIO capture 在 callback 中经过 `input_insert` 或 `bus_mix` 路由后写入监听输出；输入/输出首块 hash、地址、顺序和处理计数写入既有 runtime observation。禁用链走无转换旁路，避免实时线程承担无效 planar 转换。

验证入口：

- `native/test/test-p11-startup.ps1`：启动时间线和延迟音轨维护；可选 `-RunHost` 使用 GuitarProMCP MCP 宿主。
- `native/test/test-p11-scan-lifecycle.ps1`：静态 catalog 夹具和一次性扫描/合并证据。
- `native/test/test-p11-input-route.ps1`：`input_insert`、`bus_mix` 确定性输入路由。
- `native/test/test-p11.ps1`：汇总以上专项并输出独立 evidence JSON。

真实 Guitar Pro/ASIO 回调证据取决于本机 MCP 宿主、驱动和设备；未提供设备时，发布记录必须保留 `host_validation=false`，不得把 fixture PASS 写成真实硬件验收。
