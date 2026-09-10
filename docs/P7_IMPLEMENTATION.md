# P7 实现记录

P7 已完成本项目能够在锁定 Guitar Pro 8.1.1.17 中取得证据的范围。面板、自动清单、二态选择、实时多实例链、原生 VST3 editor bridge 和 opaque state 保存均已实现并通过隔离 MCP 宿主验证。作用域仍是已验证的 master 后处理位置；Guitar Pro 私有轨道级作用域没有可复用的公开契约，因此继续标记为宿主受限。

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

通过结果：

- `test-p7-ui.ps1`：P7 catalog UI、`enabled` 语义和隔离 editor 边界通过。
- `test-p2-runtime.ps1`：P2/P3 实时 VST3 processing 通过。
- `test-p7-mcp.ps1`：同级 panel、自动清单、单实例、双实例串联、取消单项、全部取消直通和原生 editor 通过。证据目录为最近一次生成的 `artifacts/mcp-p7-*/verification.json`。
- `test-p7.ps1`：锁定宿主文件版本、SHA-256、PE x64、标准目录异步清单和扫描超时边界通过。最近一次清单发现 20 个 bundle、加载 16 个 factory、枚举 38 个 class、11 个兼容效果器；Guitar Rig 7、Archetype Mateus Asato、Backbone、HALion Sonic bundle 在本次隔离扫描中超时并记录为错误，不阻塞其它插件清单。

## 宿主受限边界

- 私有 hook 的实际处理位置是已验证的 master 后处理点；当前没有证据证明它对应 Guitar Pro 的单轨或单音源作用域，因此不能把 P7 选择宣称为轨道级效果器链。
- 第三方 bundle 的完整生命周期仍可能因插件自己的扫描行为超出隔离进程限时；超时插件不会显示为可启用效果器，结果记录在扫描状态中。标准目录元数据清单和显式测试路径不受该限制影响。
- `IPlugView`/HWND、参数回传和 state 保存已在锁定 MCP Guitar Pro 副本中验证；未对每一个已安装插件声称 editor 兼容性。没有 editor 或不支持 HWND 的插件会在名称点击时报告不可用并保持旁路安全。
- 未运行真实声学听感、设备切换和外部输入监听验收；这些沿用 P2/P4 的宿主受限边界。

## 变更范围

核心实现位于 `native/modules/vst3_host.*`、`native/modules/gp_hook.*`、`native/modules/effect_chain.*`、`native/modules/vst3_parameters.h`、`native/modules/qt_ui.*`、`native/modules/bootstrap.cpp` 和 `native/vst3_autoload.cpp`。回归入口位于 `native/test/test-p7.ps1`、`native/test/test-p7-ui.ps1` 和 `native/test/test-p7-mcp.ps1`。
