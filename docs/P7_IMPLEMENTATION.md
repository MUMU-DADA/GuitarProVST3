# P7 实现记录

P7 当前**未完成**。已加入自动 VST3 清单、二态 sidecar 语义、最小清单 UI 和可重复入口；真实 Guitar Pro 8.1.1.17 的同级私有 QWidget 插入、按清单动态发布多实例实时链和 `IPlugView`/HWND 原生 GUI 尚未完成，不能把面板、JSON 写入或 DLL 加载当作目标完成。

## 已实现

- `native/modules/vst3_host.cpp` 去掉 `ParametricOD.vst3`、`Gateway.vst3`、`NAM Rig.vst3` 白名单，递归发现四个 Windows VST3 标准目录；按规范化模块路径和 class UID 去重，保留 audio module class 的名称、厂商、UID、模块路径及兼容性结果。
- 标准目录只做 `GetPluginFactory` 元数据扫描，避免在 Guitar Pro 进程内为仅列清单的插件创建 processor；设置 `GPVST3_VST3_PATHS` 或 `GPVST3_VST3_ROOT` 才执行 P1 生命周期探针。
- `native/modules/state_manager.cpp` 继续使用 schema 1 和 `QSaveFile`，读取旧 `bypass` 时迁移为 `enabled = !bypass`，写入时同时保留兼容字段；新 UI 只使用 `enabled`。
- `native/modules/qt_ui.cpp` 增加 P7 清单面板：只显示兼容插件名称和复选框，重名附厂商；新发现插件默认未选中；每次勾选自动保存，名称点击对未启用项无效，并对当前未验证的原生 editor ABI 显示 `host_limited`。
- 启动时不再自动显示 P7 面板。入口按钮会持续注入右侧“音源”区域；曲谱、轨道或音源侧栏重建后会自动补回。若宿主没有可识别的音源容器，则在主窗口菜单提供“VST3 效果器”入口。
- 入口按钮和菜单项现在通过面板管理函数重新获取窗口，不再捕获已关闭面板的悬空指针；面板关闭后再次点击入口会重新创建并聚焦面板。
- `native/test/test-p7-ui.ps1` 和 `native/test/test-p7.ps1` 覆盖清单 UI、`checked = enabled`、旧旁路迁移、标准目录清单、宿主文件 SHA-256/PE x64 记录和安全边界。

## 待完成计划

- 启动阶段不再同步等待全量 VST3 扫描；扫描移动到首次打开 P7 选区后的工作线程，面板显示扫描状态，完成后再刷新清单。
- 进程内缓存清单，仅在标准目录 bundle 清单变化时重新扫描，避免每次启动和每次打开重复加载全部插件 factory。
- 为启用插件持有与实时处理一致的 component/controller/processor，完成 `IPlugView`/HWND、`IPlugFrame` 和 `IComponentHandler` 桥接；参数修改必须能回到同一 processor。
- 将清单状态接入多实例实时链，验证单项启停、多项串联、取消单项和全取消直通。
- 在真实 Guitar Pro 中取得同级父容器、作用域和窗口重建证据后，才更新 P7 完成状态。

## 验证证据

```powershell
./native/build.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64
./native/test/test-p7-ui.ps1 -QtDir C:/Users/mumu/source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64
./native/test/test-p7.ps1 -PluginPath .tools/native/p7-release-build/plugins/imageformats/guitarpro_vst3_autoload.dll
```

最近一次隔离结果：标准目录发现 20 个 bundle、加载 20 个 factory、枚举 48 个 class，其中 20 个 audio effect class 出现在清单；全部宿主锁定文件为 x64，哈希与 `native/host_manifest.json` 一致。证据位于被忽略的 `artifacts/p7-c3672e5d3f444665839426a567a07089/verification.json`。

## 宿主受限边界

- 当前锁定版本只通过已有观察 hook 看到 `soundsContainer`，没有稳定、可复现的同级父布局插入契约；现有入口仍按旧 P5 生命周期探测，不能宣称同级效果器链已验收。
- P3 的双槽 callback 只支持既有单运行时效果器模型，尚未把 P7 清单映射为可安全释放的多实例串联链；P7 勾选状态会保存，但不会自动声称已产生声音处理。
- `IPlugView` 的 HWND 宿主、`IPlugFrame`、参数消息队列和 controller/component 同实例 state 回传未在真实 GP 中取得证据，名称点击明确报告 `host_limited`。

P7 因此标记为“未完成；已实现部分和关键真实宿主能力分别记录”，而不是把编译、菜单或 JSON 结果写成完整验收。
