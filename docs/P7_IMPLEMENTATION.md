# P7 实现记录

P7 本轮 R1–R4 已完成（2026-09-11）：实现、专项/真实宿主回归及发布包验收已通过。需求与验收清单维护在 [实时实现计划 P7.8](REALTIME_IMPLEMENTATION_PLAN.md#p78-剩余实施计划2026-09-10)。本轮保留此前计划新增的“扫描不得执行第三方代码”边界。

## 实现结果

### 独立原生 GUI

- 选择区及入口仍是 `soundsContainer` 的直接子级。新增 `gpvst3CloseSelectorButton` 关闭选择区，重开时重新创建；独立 editor 不属于选择区。启动只准备隐藏选择区，主动点击会在同一 Qt 事件中挂接并显示新面板，已修复销毁后需要点击两次才显示的问题。
- `gpvst3NativeEditorWindow` 使用主窗口所属的非模态 `Qt::Window`，内部 `gpvst3NativeEditorHost` 承载原生 child HWND 和正在处理音频的同一 component/controller 的 `IPlugView`。
- 重复打开复用并聚焦已有窗口；切换前保存旧 state 并释放 view。关闭 editor 继续处理，关闭/重建选择区保留 editor。取消当前效果器先关闭 view，再按既有音频读者排空流程释放实例；退出时先捕获 state，再清理 view、扫描线程和实时链。
- 插件的 `IPlugFrame::resizeView` 同时调整 Qt host 和窗口。支持 `IPlugViewContentScaleSupport` 的插件接收打开/屏幕变更时的缩放因子。当前 96 DPI 下尺寸、移动和关闭已验证；不同 DPI 多显示器组合未运行。

### 静态发现与按需识别

`native/modules/vst3_catalog.cpp` 单独实现本地发现、PE x64 检查、指纹、JSON 解析和缓存。此模块没有第三方 loader、factory、进程启动或网络调用。首次扫描、打开入口、每 60 秒后台检查、手动刷新和缓存重建均走同一路径；只运行一个工作任务，进度/部分结果通过快照交给 Qt，退出会取消并排空工作线程。

元数据取自 bundle 内 `Contents/Resources/moduleinfo.json`，校验结构、CID、类别和本地文件归属。Windows FUID 文本转换为运行时 TUID 字节顺序。只有明确的 audio effect 元数据标记为 `identified=true`；这不代表 `compatible=true` 或 processor 已验证。instrument 被过滤；缺少、损坏、不受支持的元数据保留路径及名称，并显示“待识别”，class UID 保持空值。

用户主动勾选待识别项时，只动态枚举该 bundle；宿主哈希和显式禁用配置在加载前检查。单个效果器直接进入既有启用流程，多个效果器 class 列出供选择。失败保持未启用并解释原因；刷新不会自动重试动态加载。运行时再次核对实际 factory 的类别和 class UID。待识别条目使用 `gpvst3Identify_<pathHash>`，路径 hash 仅用于控件 ID，不作为插件 class UID。

原 `rundll32/Gpvst3Scan` 路径已删除。完整生命周期探针需要显式 `GPVST3_RUN_LIFECYCLE_PROBE=1`；P1/P2 测试入口已设置该标志。单独指定 `GPVST3_VST3_ROOT` 或 `GPVST3_VST3_PATHS` 只改变静态目录范围。

### 缓存和反馈

唯一派生文件是 `state::dataDirectory()/vst3-catalog-cache.json`。记录 schema、扫描器版本、x64、规范化 roots、bundle 文件清单、二进制大小/修改时间、元数据内容指纹、名称/厂商/类别/UID、来源、识别状态、错误与重试时间。不同 roots 在同一文件内分 scope，标准目录和开发路径隔离；通过 `QSaveFile` 原子写入。

启动先读缓存供列表使用，再后台静态检查；未变化条目复用解析结果，新增/变化条目静态重查，删除项移出发现结果。读取/解析失败缓存 60 秒，文件变化可立即重试。损坏/旧 schema 自动重建，写入失败仍展示可取得结果；扫描结果不清空 sidecar 的启用意图和 opaque state。派生候选项不写入 sidecar，运行参数仍只由 `effect-chain.json` 管理。

入口按钮覆盖首次扫描、已知数量进度、缓存更新、完成、部分失败和全失败重试。界面刷新不重新创建正在处理的实例或 editor；加载单个插件的提示与扫描进度分开。

## 本轮验证（原软件免安装）

所有真实宿主运行使用正式目录 `C:/Program Files/Arobas Music/Guitar Pro 8/GuitarPro.exe` 和仓库开发 DLL。每次记录实际 PID、DLL 路径/哈希及安装目录完整性；测试数据、客户端连接信息和截图均留在忽略的 `artifacts/`，未提交。

| 验证 | 结果与证据 |
| --- | --- |
| Qt 夹具 | 独立窗口/尺寸、重复打开、关闭后仍启用、选择区实际析构后单击重建/显示、扫描按钮状态和失败回滚通过。P5 Qt 面板回归同时通过。`.tools/native/p7-ui-test` |
| 静态缓存夹具 | 19 个场景通过：冷/热缓存、二进制/元数据变化、增删、错误重试、损坏/旧 schema/写失败/恢复、空目录、scope 隔离、instrument/非法 CID/不支持格式，以及非本地根目录在 IO 前拒绝；sidecar 字节不变。`.tools/native/p7-catalog-test/verification.json` |
| 标准目录 + 默认启用 | 发现 20 个候选，静态扫描 54 ms，第三方模块加载数 0。该机器的安装包均缺少有效静态效果器元数据，因此首扫显示 20 个“待识别”；其中 NAM Rig 的 JSON 有尾随逗号，保留候选并报告解析失败。`artifacts/mcp-p7-58c8a36f6c734b9892bf0cf6e882af3d/verification.json` |
| 原生 GUI 与链 | ParametricOD/Gateway 单/双实例、关闭/重开/切换窗口、关闭并重建选择区、单项停用、全部直通和 state 恢复通过。在显示 GP 主窗口及音轨侧栏的情况下，editor 打开前后选择区均为 234×261，位置均为 (0,30)；ParametricOD state 52 字节、Gateway 151 字节。同上证据及 `editor-native.png`、`second-editor-native.png` |
| 标准目录跨进程缓存与可见列表 | 冷扫描 54 ms、重启检查 45 ms，均检查 248 个文件；重启复用 20/20 个 bundle，元数据解析 1→0 次。冷/重启点击入口到列表读回为 194/251 ms（冷启动查询含 100 ms 等待）；重启的 20 个复选框均可见且 enabled，空启用链的实际 `.vst3` 模块为零。修复前曾复现新面板首击隐藏；本行证据使用修复后的发布构建。同上 `default_catalog_restart` |
| 跨 GP 进程缓存与入口标记 | 冷/重启列表控件查询耗时 31/31 ms；静态扫描为 7/6 ms，重启复用 2 个 bundle，文件检查数均为 6。两个 marker bundle 的 `DllMain/InitDll/GetPluginFactory` 在首扫、刷新和重启阶段执行记录为零；主动选择后只有一个 bundle 产生实际 PID 入口记录，null factory 明确失败，其余条目未加载，刷新不重试。`artifacts/p7-static-host-3118cf6e37ad4c53900141edddb30cf6/verification.json` |
| 扫描中按钮与合并请求 | 在独立慢检查测试构建中，MCP 实际读取 `VST3 · 扫描 0/2`、重启时的 `VST3 · 正在更新…`；连续点击 generation 保持 1，完成后恢复 `VST3`。`artifacts/p7-static-host-afa5d916ae024c909380251194f578e3/verification.json` |
| 全失败与重试 | 读取被拒绝的非本地根目录时显示“扫描失败，点击重试”；重试创建新 generation 并进入终态，不进行网络文件读取。`artifacts/p7-static-host-978e8c4ab3494ffab0e0e87cb8b34c3c/verification.json` |
| 扫描中退出 | 在扫描仍 pending 时请求 GP 正常退出，工作线程取消/排空，退出码 0，未强杀。`artifacts/p7-static-host-7af66eb87f884ee4b36fa83d260aa526/verification.json` 及 `cold/shutdown.json` |
| GUI 参数到实际音频 | 测试 VST3 的原生 Qt view 经 MCP 将增益改为 0.25，`performEdit` 被同一 processor 消费；RSE 输入能量 62.184926、输出 3.886558，比值 0.0625。原生 resize/move 及跨 GP 重启的启用链和 0.25 参数恢复通过。`artifacts/mcp-p7-bff82a46bb88482a84b1a9f2d6b6d67c/verification.json` |
| 显式禁用 | `GPVST3_ENABLE_P2_HOOK=0` 在单项识别前拒绝加载，保留未启用状态并报告实际原因。`artifacts/mcp-p7-8b7dc8e712b149458047653ee3966aed/verification.json` |
| P0/P1 与 P6 宿主门控 | 宿主哈希不匹配时拒绝 hook/实时模式、P1 生命周期及五个锁定文件的正负向门控通过。`artifacts/p0-a637cdfee8dc49e9adc4948097b742fe`、`artifacts/p0-b10d0e0b2c2f424d99aea708a0a70591`、`.tools/native/p6-gate-test` |
| P6 包与安装回归 | 清单哈希、zip 清单、重复安装、未拥有文件保护、已修改文件拒绝卸载及正常卸载通过。`artifacts/p6-package-a90d85c484c64051af8c3699b25ff500` |
| P2/P3 实时链回归 | 通过。`artifacts/p2-runtime-d1c511b2aa254707b1fffceae8ebf11f/verification.json` |

P7 默认/参数恢复/静态缓存测试的 GP 退出均已核对 `shutdown.json` 为 `forced=false, exit_code=0`。MCP 连接可能在退出响应送达前关闭；脚本用同一进程的退出结果判定终态，不重发关闭操作。

网络记录采用按 GP PID 采样的 TCP 连接表，见静态宿主证据中的 `network`；GP 自身活动及 MCP loopback 与第三方入口记录分开归因。采样不是完整抓包，也不证明整个 GP 离线。扫描的零第三方执行结论由静态模块的调用边界及带可观察入口的 VST3 在首扫/重扫/重启中的记录共同支持。主动选择/已启用链恢复属于运行时流程，第三方代码仍可能自行联网。

MCP 的 `gp_windows` 可证明窗口身份、父关系、可见性、非模态和几何；`gp_screenshot` 对部分第三方 HWND 返回黑图，不能当作原生内容证据。本轮用 `p7-window-capture.ps1` 对实际测试 GP PID、已观察标题的 HWND 调用 `PrintWindow`，已人工检查 ParametricOD/Gateway 截图；没有使用 `computer-use`。测试增益插件由本项目提供，仅用于可观测参数/音频验证，不代替第三方 GUI 兼容性结论。

## 复现命令

```powershell
./native/build.ps1
./native/test/test-p7-catalog.ps1
./native/test/test-p7-ui.ps1
./native/test/test-p7-mcp.ps1 -HookMode default -StandardScan -CheckCatalogRestart
./native/test/test-p7-mcp.ps1 -HookMode disabled
./native/test/test-p7-static-host.ps1
./native/test/test-p7-static-host.ps1 -ExpectFailure
./native/test/build-p7-gain-fixture.ps1
./native/test/test-p7-mcp.ps1 -HookMode default -CheckGain -Vst3Root 'C:/Users/mumu/source/GuitarProVST3/.tools/native/p7-gain-test/P7 Gain Fixture.vst3;Gateway.vst3'
./native/test/test-p2-runtime.ps1
./native/test/test-p7.ps1
./native/test/test-p0.ps1 -RejectHostFile GPRSE.dll
./native/test/test-p1.ps1
./native/test/test-p6-gate.ps1
./native/build.ps1 -CatalogDelayMs 4000 -OutputRoot .tools/native/p7-progress
./native/test/test-p7-static-host.ps1 -PluginPath .tools/native/p7-progress/plugins/imageformats/guitarpro_vst3_autoload.dll -ObserveProgress
./native/test/test-p7-static-host.ps1 -PluginPath .tools/native/p7-progress/plugins/imageformats/guitarpro_vst3_autoload.dll -ObserveProgress -ExitDuringScan
./native/test/test-p6-package.ps1
./native/package.ps1 -Version 0.7.0
```

最终正常构建 DLL 的 SHA-256 为 `897401FA71838ECC27E6849FACEBA57B0DFD14FB69F8C92509D558B1E2F42BC9`。界面首次点击修复后的标准目录/可见列表/重启、参数音频、显式禁用和 marker 验收均使用此 DLL；其余早期证据按各记录中的构建哈希保留。

`-CatalogDelayMs` 仅用于独立测试构建，不进入正常构建/发布 DLL。测试 marker、增益 VST3、缓存、客户端配置及截图不随包分发。发布包为 `artifacts/release-p7-0.7.0-20260911/GuitarProVST3-0.7.0.zip`；该目录的 `verification.json` 保存包 SHA-256 和源 DLL 哈希，`package-whitelist.json` 保存文件白名单核对结果。安装目录未因本轮开发/发布测试而改写。

## 保留的宿主边界

实际 hook 仍是已验证的 master 后处理点，未取得轨道级/单音源作用域。真实扬声器听感、设备切换、外部输入监听继续按 P2/P4 标记为宿主受限；本轮不扩大这些结论。第三方插件主动加载后的原生崩溃、长时间阻塞和所有 editor 的兼容性不由扫描缓存提供隔离保证。

## 历史记录

以下 2026-09-10 记录使用旧动态扫描和当时的 editor 行为，保留用于追溯；新的静态扫描与独立窗口结论以上表为准。

## 正常启动后的勾选修复（2026-09-10）

已确认用户安装目录中的两项问题：默认启动没有设置 `GPVST3_ENABLE_P2_HOOK`，选择在创建实例前即被拒绝；`Program Files` 路径含空格时，`QProcess` 把 DLL 路径和 `,Gpvst3Scan` 一起加引号，导致 `rundll32` 未执行扫描入口，所有 bundle 返回 `scan_worker_failed`，界面只能显示 sidecar 中的旧记录。

已实现：第一次启用 P7 插件（包括恢复已启用的 sidecar）会重新校验宿主哈希，再按既有 prologue 门控安装实时接入。没有启用项的默认启动仍保持无 hook、旁路；显式 `GPVST3_ENABLE_P2_HOOK=0` 继续禁止启用。默认启动也注册后续接入需要的观测和退出清理。勾选失败会回滚，并区分宿主不兼容、启动配置禁用、接入失败、插件缺失和状态恢复失败，具体错误码保留在提示的 tooltip 中。扫描命令改为只给 DLL 路径加引号。

已验证：旧 DLL 在默认启动 MCP 回归中复现勾选失败；修复后，在含空格的隔离 Guitar Pro 路径中，不设置 hook/扫描开发开关即可发现 12 个兼容效果器，并完成 ParametricOD/Gateway 单实例、双实例串联、原生 editor、全部停用及状态恢复。本次扫描有一个第三方 bundle 超时；扫描成功不代表全部插件的处理或 GUI 兼容性。显式禁用配置及 UI 错误回滚也已通过专项回归。

Archetype Mateus Asato 的专项回归另发现：组件 `setState` 成功后，控制器 `setComponentState` 返回 `kNotImplemented`，旧实现因此拒绝再次启用。已修复为允许控制器不实现该复制步骤，组件恢复及其他控制器错误仍严格检查；恢复失败 tooltip 包含具体步骤和原始返回值。使用显式路径的真实宿主回归已通过该插件的启用、串联处理、原生 editor、停用和再次恢复。其标准目录扫描仍可能触发 10 秒超时，不承诺每次扫描均发现该插件。

```powershell
./native/test/test-p7-mcp.ps1 -HookMode default -StandardScan
./native/test/test-p7-mcp.ps1 -HookMode disabled
./native/test/test-p7-mcp.ps1 -HookMode default -Vst3Root 'Neural DSP/Archetype Mateus Asato.vst3;Gateway.vst3'
./native/test/test-p7-ui.ps1
```

证据：`artifacts/mcp-p7-744c6f82a6fa460a8a50a937de122e49/verification.json`、`artifacts/mcp-p7-b143fbb5e3894fecbeec3e0b20dfce30/verification.json`、`artifacts/mcp-p7-ba41f7526091478fb1c3b05aea5b84ff/verification.json`。宿主哈希不匹配拒绝接入的回归通过，证据为 `artifacts/p0-f5a1f326b4ea4579947b60905f798f6f/verification.json`。真实扬声器听感未验证。

最终 DLL 的默认启动和标准目录复核通过，证据为 `artifacts/mcp-p7-68f61605223a4540b9465b4d825c7018/verification.json`；本次 Guitar Rig 7 和 Mateus Asato 扫描超时，其余清单及 ParametricOD/Gateway 回归通过。回归脚本等待侧栏实际挂接完成，避免恰好在 500 ms 重建定时器执行前断言。已在 Guitar Pro 关闭后备份并更新本机安装 DLL，安装文件与验证构建的 SHA-256 一致；安装记录位于 `artifacts/installed-selection-fix-62fe9f2097494a4999501536a9e16104/verification.json`。

## 变更范围

产品代码：`native/modules/vst3_catalog.cpp`、`vst3_host.*`、`qt_ui.*`、`gp_hook.*`、`bootstrap.cpp` 和 `native/vst3_autoload.cpp`。测试入口及夹具位于 `native/test`；构建脚本、安装说明与实时计划同步更新。
