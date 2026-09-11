# P8：音轨级与全局 VST3 范围与验收

状态：P8 六项补齐工作及交付验收已完成（2026-09-11，Guitar Pro 8.1.1.17 / Windows x64）。实际实现和证据见 [P8 实现记录](P8_IMPLEMENTATION.md)。本文件只保留当前有效的范围、数据模型和验收结论；原始长计划保留在源码仓库归档中。

## 范围

- 后台静态发现和主动识别分离；识别中、失败和超时条目不进入可操作列表。
- `global` 链作用于 GP Master；`track` 链作用于已解析的对应音轨；两者实例、参数、state、顺序和 editor 独立。
- 已启用项在各自列表前部显示，拖动或 `Alt+Up` / `Alt+Down` 决定实际处理顺序。
- 音轨增删、重排、保存/另存、关闭重开和进程重启恢复对应链及参数。

## 数据与运行时模型

- `effect-chain.json` 使用 schema 2：`global.effects` 保存 Master 链，`scores.<score>.tracks.<persistent-key>.effects` 保存音轨链；schema 1 顶层 `effects` 迁移到 global 并保留兼容视图。
- 音轨持久化 key 与 document ID 组合，避免不同曲谱共享运行时实例；同一会话内的原生 track ID 用于增删、重排和撤销关联。
- `processDSP` 先调用 GP 原函数，再处理对应音轨的 `IAudioBuffer`；global 链在 GP Master 后处理点运行。未解析上下文旁路，不借用其他音轨实例。
- 发布 DLL 可通过 GuitarProMCP bridge 或独立 native collector 发现音轨；MCP bridge 是可选 provider。

## UI 结构

- track 区挂在 GP 原生 `soundRack` 后，global 区挂在 `soundMastering` 后；两区各自拥有 wrapper、列表和可见 `QFrame::HLine` 分界线。
- 稳定 objectName 包括 `gpvst3TrackVst3Section`、`gpvst3GlobalVst3Section`、`gpvst3TrackChainList`、`gpvst3GlobalChainList` 和 `gpvst3AvailableList`。
- 窄侧栏 260/320/420 px、100%/125%/150% 缩放下，名称保持左对齐并以 tooltip 提供完整身份；复选框、排序和 GUI 控件保持可用。

## 验收结论

| 项目 | 结果 |
| --- | --- |
| 后台识别与 10 秒超时 | 真实慢 factory 被忽略，队列继续，缓存可复用，手动刷新可重试 |
| 音轨运行时 | 双轨独立实例和 buffer 写回通过；完整生命周期恢复通过 |
| 原生音源链 | 挂载前后原生效果数组一致，原生 EQ 参数/旁路/播放通过 |
| global/track 分区 | 真实宿主父子关系、geometry、visibility 和分界线断言通过 |
| 顺序与 P4 组合 | track/global 顺序、重启恢复及 `input_insert` / `bus_mix` 组合通过 |
| 发布路径 | 无 MCP bridge 的发布构建可完成音轨发现、恢复和处理 |

## 保留限制

实机音频设备矩阵、真实扬声器听感、capture 监听/反馈、其他 GP 版本和第三方插件崩溃恢复仍未完成；这些限制不影响已验证的 P8 功能结论。
