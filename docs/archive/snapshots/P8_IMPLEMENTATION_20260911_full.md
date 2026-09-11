# P8 实现记录

P8 六项补齐工作及交付验收已完成（2026-09-11，Guitar Pro 8.1.1.17 / Windows x64）。`EffectsChain self -> track ID` 和可写 `IAudioBuffer` 已用于真实音轨处理；音轨链不再以 ABI 未解析为由整体旁路。验收范围与目标见 [P8 计划](P8_TRACK_GLOBAL_VST3_PLAN.md)。

## 当前状态

| 阶段 | 实现与验证结果 |
| --- | --- |
| P8.7 后台识别 | queued/running/failed/timeout 不进入可操作列表；真实 11 秒 factory 在 10 秒截止后被忽略，队列继续，最后一项超时也会结束识别状态。自动刷新保留超时和成功识别缓存，手动刷新可重试超时项。 |
| P8.8 音轨运行时 | 双轨独立实例、GUI、参数及实际 buffer 写回；未命名曲谱首次保存、Save As、增删/重排/撤销、关闭重开和进程重启后的全轨自动恢复通过。MCP bridge 与独立 native collector 均有生命周期证据。 |
| P8.9 原生音源链 | 原生音色/效果数组在挂载 VST3 前后相同；真实 GP 的 `E30_EqGEq` 可修改参数、切换旁路并恢复，随后正常播放。 |
| P8.10 分区域 UI | 实际 track 列表及操作控件挂在 `soundRack` 后，global 列表挂在 `soundMastering` 后；各自 wrapper 和可见 `QFrame::HLine` 分界线通过真实宿主父子关系、geometry、visibility 和 Qt 布局断言。 |
| P8.11 窄侧栏 | 260/320/420 px，100%/125%/150% 缩放下名称前缀、省略、完整身份 tooltip、复选框/GUI 不重叠通过；真实列表移动、键盘/菜单排序、侧栏销毁重建保留两条链。 |
| P8.12 联调交付 | 核心套件、双轨生命周期、独立发布运行、真实 global/track/P4 组合、P7 回归及发布包归属/卸载核对通过。PowerShell 语法、宿主哈希正反例、`git diff --check` 和敏感/产物文件检查通过。 |

## 实现

- 目录扫描只读取本地文件和缓存，不执行第三方代码；信息不足的 bundle 由独立后台 worker 调用 factory。识别成功立即发布 `recognition_status=ready`，失败发布 `failed`，超时标记 `timeout` 并丢弃迟到结果。正在运行的第三方调用不强制杀线程，宿主退出不等待它；识别取消/丢弃与进程隔离是不同能力。
- schema 2 用 `global.effects` 和 `scores.<score>.tracks.<persistent-key>.effects` 保存独立 state。schema 1 原链迁移到 global，保留 opaque state、enabled 和顺序。
- 打开曲谱时按已保存的 `track_index` 关联唯一的持久化记录；同一会话内增删、重排和撤销跟随原生 track ID，运行时 key 是 document ID 与持久化 key 的组合。新增音轨取得新记录，删除记录保留以供撤销；Save As 复制到新曲谱记录且复用正在运行的实例。所有音轨自动恢复，不要求逐轨打开界面。
- `gp_audio_runtime` 优先读取版本为 1 的 GuitarProMCP 进程内 bridge；没有 bridge 时由本 DLL 的 Qt 对象生命周期观察器发现文档、Conductor、Musician、Track 和 EffectsChain。直接使用 GPCore/GPRSE 的最小导入声明，发布 DLL 不依赖 `guitarpro_mcp.dll` 或该仓库的导入库。
- `dspProcessHook` 先执行 GP 原函数，再对解析到的对应音轨缓冲执行 VST3；global 链在 GP Master 后处理点执行。每轨有独立双槽链、预分配 scratch、处理计数及 writeback 标志，dispatch 更新使用 generation/reader drain。`EffectsChain::index()` 只作诊断，未知上下文不会借用其他音轨的实例。
- 已启用插件按列表顺序串联；重新排序复用实例。控制线程处理采样率重配置、保存状态和单项故障隔离；重配置期间不对错误采样率的块执行插件。缺失插件或损坏 state 不清空其他健康插件，单项 process 失败后禁用该项并恢复剩余链。
- 现有容量是最多 32 个活动音轨运行时、64 个 chain binding，每链最多 8 个效果器，mono/stereo、最多 16384 帧。越界或未确认的上下文有明确拒绝/旁路结果。

## 验证入口

在 Windows x64、真实 Guitar Pro 8.1.1.17 上原软件免安装验证；只改变测试子进程的环境和仓库内测试数据。主机安装目录前后 SHA256 相同，下面列出的成功宿主均正常退出（exit code 0，未强制结束）。

```powershell
./native/build.ps1 -OutputRoot .tools/native/p8-track-build
./native/test/test-p8.ps1 -OutputRoot .tools/native/p8-delivery-suite
./native/test/test-p7-ui.ps1
./native/test/test-p8-recognition-host.ps1
./native/test/build-p7-gain-fixture.ps1
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -Vst3Root 'C:/Users/mumu/source/GuitarProVST3/.tools/native/p7-gain-test/P7 Gain Fixture.vst3' -ExpectedBindingSource mcp_bridge
./native/build.ps1 -ForceNativeAudioBindings -OutputRoot .tools/native/p8-native-build
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -Vst3Root 'C:/Users/mumu/source/GuitarProVST3/.tools/native/p7-gain-test/P7 Gain Fixture.vst3' -PluginPath .tools/native/p8-native-build/plugins/imageformats/guitarpro_vst3_autoload.dll -ExpectedBindingSource native_document_registry
./native/test/test-p8-order.ps1 -P4Route input_insert
./native/test/test-p8-order.ps1 -P4Route bus_mix
./native/test/test-p7-mcp.ps1 -PluginPath .tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll -HookMode default -CheckGain -Vst3Root 'C:/Users/mumu/source/GuitarProVST3/.tools/native/p7-gain-test/P7 Gain Fixture.vst3;Gateway.vst3'
./native/test/test-p6-package.ps1 -PluginPath .tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll
git diff --check
```

`test-p8.ps1` 覆盖 state、recognition、timeout、UI 和 runtime 五组；独立执行的真实宿主脚本不能由该套件的 PASS 替代。`test-p8-standalone.ps1` 接受生命周期脚本生成的 `-ScorePath .../renamed-tracks.gp`、`-SidecarPath .../effect-chain.json` 和 `-Vst3Root`，用测试专用 Qt driver 打开/播放曲谱，断言进程完全没有 MCP bridge。

`-ForceNativeAudioBindings` 仅用于验证 fallback 的单独构建，不打入发布包。测试 VST3、driver、开发 DLL、缓存和 artifacts 不纳入提交或发布。

## 核心证据

| 验收 | 本地证据 | 实际结果 |
| --- | --- | --- |
| 真实超时、终态与缓存 | `artifacts/p8-recognition-host-372d4b7636a44d2fac8e5a60c807eb12` | cold/queue 各有一个真实 10 秒超时；Qt/MCP 响应最大 286/253 ms；两个重启进程均复用 2 个 bundle，识别调用 0；退出 303–427 ms，其中 cold 在慢 factory 未返回时退出。识别出的 3 个效果器在实际 global 列表中可操作。 |
| MCP bridge 生命周期及原生链 | `artifacts/mcp-p8-track-953cf3fa6ff844dc8ce47bf407575db6` | 两轨实际能量比为 0.0625/0.25，对应 0.25/0.5 增益；global 独立增益 0.75。音轨变更、首次保存、另存、关闭重开、重启及原生 EQ 参数/旁路回读通过。 |
| 独立 collector 生命周期 | `artifacts/mcp-p8-track-05ceb616fdb34b4daec0463c23ecf968` | 强制 native collector 后重复同一完整生命周期回归，MCP 仅负责驱动测试动作。 |
| 发布路径无 MCP | `artifacts/p8-standalone-fef81c8c1c2848778fbb4e5cab0b5635` | 发布构建自身发现并恢复两轨/global；MCP 模块列表为空；两轨各处理并写回 6 blocks，global 9 blocks，零错误。 |
| global/track + P4 | `artifacts/mcp-p8-order-18aa88aa5ddf4bc384fb4ce495392a4e`、`artifacts/mcp-p8-order-a2268686d6414287b0240b49e8e8d94c` | 分别验证 input_insert/bus_mix，与双 scope 排序、刷新、重新启用及重启组合；实际 callback 的三样本满足下方公式。 |
| 采样率和故障 | `artifacts/p8-runtime-b809b7be2516437d9a9b24469ec18ea1` | 在真实 GP 内加载实际测试 VST3，用确定性缓冲验证 44100/48000/96000/44100 Hz、实例保留、缺失插件、坏 state 和单项 process 失败隔离。采样率 accessor/发现由夹具替换，该项不是设备切换实测。 |
| Qt 与 state 基础套件 | `.tools/native/p8-delivery-suite.log` | schema 迁移、身份恢复、后台识别、过期静态重试不重载有效 factory cache、手动恢复超时、实际分区域内容、窄宽度/DPI/重建和 P7 UI 通过。 |
| P7 真实宿主回归 | `artifacts/mcp-p7-8e2ddcd173b94542b50c32e24899f952` | Gain Fixture + Gateway：双插件启停、独立窗口、参数改变真实输出、窗口移动/缩放/关闭、选择区销毁后处理继续、state 和重启恢复通过。 |
| 发布包与版本门控 | `artifacts/p6-package-0902367558994c049528ac899c78d03b`、`.tools/native/p8-delivery-gate/verification.json` | ZIP/清单内容与哈希一致；拒绝覆盖无归属 DLL，重复安装幂等，拒绝卸载被修改的文件，恢复后安全卸载。锁定宿主通过，五个哈希不匹配及缺失文件被拒绝。 |

上述发布构建的 SHA256 为 `0BB6CF905F5894511FA3939448776F7102FDE24C4AE5CF7E9E00244EA3818CDF`；native collector 强制验证使用单独的测试构建。P6 包装测试的版本标签是 `0.6.0-test`，P8 正式打包入口的默认版本是 `0.8.0`，两者使用相同发布 DLL。

## global / track / P4 顺序

GP 的音轨路径为原生音源效果链 → 对应 track VST3 → GP 混音 → GP Master 效果 → global VST3。P4 位于原始 PortAudio callback 返回之后，使用当次 callback 的 capture 与已生成输出；不保留借用指针。

排序夹具 A 加 0.125、B 乘 0.5、C 加 0.25。ABC 的结果为 `0.5*x + 0.3125`，CAB 为 `0.5*x + 0.1875`；真实宿主验证每个 processor 的前后 buffer hash、调用序列和样本值，实例 ID 保留，保存顺序与调用顺序一致。

P4 组合测试复用 A 作为输入效果器，并从同一次设备 callback 复制 capture、GP generated、路由后 output 三个样本：

- `input_insert`：`output = capture + 0.125`。
- `bus_mix`：`output = capture + generated + 0.125`。

三样本只在 track/global 均已处理且 GP 输出非静音时记录，同时检查前后 buffer hash 改变、P4 无错误及实际写回。global 的作用域仍是 GP master；外部输入在 P4 层处理，不会再次经过 global。

## 已验证范围以外

本轮解决的是 P8 六项功能和交付缺口，真实音轨映射与可写缓冲已验证。不同 ASIO/WASAPI 设备、真实扬声器听感和 capture 监听/反馈稳定性没有实机矩阵，不能由样本写回推导为已验证。第三方插件进程崩溃隔离/恢复未实现；第三方 factory 超时采用丢弃结果，不提供 helper 进程终止保证。其他 GP 版本需要单独适配与验证，现有哈希门控继续拒绝不匹配版本。
