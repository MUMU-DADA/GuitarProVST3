# P10 实现记录

状态：已完成代码与可重复验证；实机反馈表明仍需继续收敛（2026-09-12）
关联计划：[P10：ASIO 实时链路、加载性能与音轨身份计划](P10_ASIO_LATENCY_INPUT_PLAN.md)
基线：Guitar Pro 8.1.1.17 / Windows x64。

## 交付内容

本轮修正：ASIO stream callback 在输入链关闭/旁路时提前返回，跳过原先每块必做的 interleaved→planar 转换；音轨上下文全量发现不再参与启动关键路径，首帧展示后延迟 1.5 秒才开始维护刷新。输入链现在默认启用并使用 `input_insert`；设置 `GPVST3_ENABLE_P4_INPUT=0` 或 `GPVST3_P4_ROUTE=disabled` 可显式关闭。

| 范围 | 状态 | 实现与证据 |
| --- | --- | --- |
| P10-1 启动与曲谱加载 | 已实现、已验证 | `vst3_host::beginAsync` 继续在 worker 扫描；新增 `startup_timeline`，启动阶段只恢复 UI 和已配置链；`test-p10-startup.ps1` 通过。 |
| P10-2 ASIO deadline 与零额外旁路 | 已实现、已验证 | callback 旁路提前返回，不解析设备结构、不复制 capture；`effect_chain` 和 stream callback 记录 deadline、p95/p99、超时和 copy；MCP 宿主回归记录到 `p2-observation.json.gp_hook.audio_deadline`。 |
| P10-3 输入进入 VST3 链 | 已实现、已验证 | interleaved float32 capture 在原生 callback 后进入独立 input router，支持 `input_insert`/`bus_mix`、通道适配和 output writeback；MCP 宿主两种路由均观察到处理块、写回和连续 buffer 地址。 |
| P10-4 音轨身份显示 | 已实现、已验证 | `gpvst3SoundEffectChainButton` 显示 `Track N · VST3 (N)`，tooltip/accessibility 保留完整摘要；轨道切换后刷新；`test-p10-ui.ps1` 通过。 |
| P10-5 基线与发布 | 已实现、已验证 | `test-p10.ps1` 串联 baseline/startup/ASIO/input/UI；package 白名单加入 P10 文档并排除 `.tools`、`artifacts`、测试插件和构建产物。 |

## 诊断字段

`gp_hook.audio_deadline` 包含 callback blocks、processing/max/p95/p99 nanoseconds、设备 block deadline、overruns 和额外 copy。`gp_hook.roundtrip_latency` 拆分设备输入/输出建议延迟、VST3 报告延迟和适配器延迟；`measured=false` 表示没有硬件 loopback，`status=reported_components_only` 只代表可读取的报告组成。`gp_hook.input_route` 与 `input_*` 字段记录 capture 格式、通道布局、借用所有权、处理/旁路/错误块、前后 hash 和 output writeback。

## 验证记录

已执行并通过：

- `./native/build.ps1 -OutputRoot .tools/native/p10-build`
- `./native/test/test-p10.ps1 -OutputRoot .tools/native/p10-delivery-suite`
- `./native/test/test-p8-track-runtime.ps1 -PluginPath .tools/native/p10-build/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root <P7 Gain Fixture.vst3> -P4Route input_insert`
- 同上命令使用 `-P4Route bus_mix`
- P4 router、P9 switch/UI 及完整 P8 track runtime 回归

MCP 宿主流程使用已安装 Guitar Pro 原进程和 MCP bridge，通过 `host-session.ps1` 注入开发 DLL，没有复制或改写宿主安装目录。实机硬件 loopback、不同 ASIO 驱动的设备矩阵和扬声器听感仍由设备条件决定；缺少这些条件时只报告报告延迟和 callback deadline，不把夹具 PASS 解释成硬件 roundtrip 已测量。
