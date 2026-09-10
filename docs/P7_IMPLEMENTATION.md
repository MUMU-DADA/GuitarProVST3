# P7 实现记录

P7 当前为**进行中（2026-09-10 按最新交互要求更新）**。已有面板、自动清单、二态实时链、原生 VST3 editor bridge 和 opaque state 保存实现及历史回归记录。原生 GUI 独立窗口、跨重启的扫描缓存、首次扫描按钮提示仍待实现和验收；完整剩余计划维护在 [实时实现计划 P7.8](REALTIME_IMPLEMENTATION_PLAN.md#p78-剩余实施计划2026-09-10)。

当前 `NativeEditorWindow` 使用 `Qt::Widget` 并属于 `gpvst3P7Panel`，`g_cachedScan` 只保存在内存中，扫描子进程的临时 `catalog.json` 不会供下次启动复用；入口按钮目前固定显示 `VST3`。这些现状不满足最新三项要求。以下旧 editor 父级关系、状态文字和扫描回归保留为历史证据，不能据此宣称独立窗口、缓存提速或首次按钮反馈已完成。

实际处理位置仍为已验证的 master 后处理点；轨道级作用域继续标记为宿主受限。

## 已实现和已验证

- `native/modules/vst3_host.cpp` 递归发现 Windows x64 VST3 标准目录，按规范化 bundle 路径和 class UID 去重，过滤 instrument class，异步发布兼容 audio effect 清单。标准目录扫描通过 `rundll32` 导出的 `Gpvst3Scan` 在隔离进程中执行，每个 bundle 有限时；显式设置 `GPVST3_VST3_PATHS`/`GPVST3_VST3_ROOT` 时保留完整生命周期探针。
- P7 面板和“VST3”入口在 MCP 真实宿主中确认是 `soundsContainer` 的直接子级。稳定 objectName 为 `gpvst3P7Panel`、`gpvst3Enabled_<classId>`、`gpvst3Editor_<classId>` 和 `gpvst3NativeEditorHost`；音源区域重建时会重新挂接，面板关闭后入口不会保留悬空指针。
- 复选框语义为 `checked = enabled`。启用一个插件会创建实际 component/processor/controller；两个插件按清单顺序串联；取消一个插件会复用仍启用的实例，剩余实例继续处理；全部取消后 `chain_active_slot = -1`、`total_bypass = true`。链使用预分配 planar pipeline、双槽原子发布和音频读者排空。
- 原生 GUI 使用处理链中同一实例的 `IEditController::createView("editor")`、`IPlugView`、child HWND、`IPlugFrame` 和 `IComponentHandler`。组件/控制器连接、参数 mailbox、`restartComponent` 和 state capture/restore 已实现。MCP 证据中 `gpvst3NativeEditorHost.parent_name = gpvst3P7Panel`，状态显示“原生 GUI 已打开：ParametricOD”，`runtime_effect_error` 为空。
- sidecar 使用 `enabled`、module/class UID、component state 和 controller state；旧 `bypass` 读取时迁移为 `enabled = !bypass`，关闭 GUI、取消选择和宿主退出时自动保存。最近一次 MCP sidecar 中 Gateway state 为 151 字节、ParametricOD state 为 52 字节。

## 验证命令

```powershell
./native/build.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64 -OutputRoot .tools/native
./native/test/test-p7-ui.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64
./native/test/test-p2-runtime.ps1 -PluginPath .tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/test/test-p7-mcp.ps1 -PluginPath .tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll
./native/test/test-p7.ps1 -PluginPath .tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll -ScanTimeoutSeconds 90
```

历史回归结果（本轮计划调整尚未执行新增验收）：

- `test-p7-ui.ps1`：P7 catalog UI、`enabled` 语义和隔离 editor 边界通过。
- `test-p2-runtime.ps1`：P2/P3 实时 VST3 processing 通过。
- `test-p7-mcp.ps1`：同级 panel、自动清单、单实例、双实例串联、取消单项、全部取消直通和原生 editor 通过。证据目录为最近一次生成的 `artifacts/mcp-p7-*/verification.json`。
- `test-p7.ps1`：锁定宿主文件版本、SHA-256、PE x64、标准目录异步清单和扫描超时边界通过。最近一次清单发现 20 个 bundle、加载 16 个 factory、枚举 38 个 class、11 个兼容效果器；Guitar Rig 7、Archetype Mateus Asato、Backbone、HALion Sonic bundle 在本次隔离扫描中超时并记录为错误，不阻塞其它插件清单。

## 宿主受限边界

- 私有 hook 的实际处理位置是已验证的 master 后处理点；当前没有证据证明它对应 Guitar Pro 的单轨或单音源作用域，因此不能把 P7 选择宣称为轨道级效果器链。
- 第三方 bundle 的完整生命周期仍可能因插件自己的扫描行为超出隔离进程限时；超时插件不会显示为可启用效果器，结果记录在扫描状态中。标准目录元数据清单和显式测试路径不受该限制影响。
- `IPlugView`/HWND、参数回传和 state 保存已在锁定 MCP Guitar Pro 副本中验证；未对每一个已安装插件声称 editor 兼容性。没有 editor 或不支持 HWND 的插件会在名称点击时报告不可用并保持旁路安全。
- 未运行真实声学听感、设备切换和外部输入监听验收；这些沿用 P2/P4 的宿主受限边界。

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

核心实现位于 `native/modules/vst3_host.*`、`native/modules/gp_hook.*`、`native/modules/effect_chain.*`、`native/modules/vst3_parameters.h`、`native/modules/qt_ui.*`、`native/modules/bootstrap.cpp` 和 `native/vst3_autoload.cpp`。回归入口位于 `native/test/test-p7.ps1`、`native/test/test-p7-ui.ps1` 和 `native/test/test-p7-mcp.ps1`。
