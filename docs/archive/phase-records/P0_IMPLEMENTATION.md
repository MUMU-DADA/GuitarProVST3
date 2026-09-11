# P0 实现记录

P0 建立插件自动加载入口、宿主版本锁定和默认旁路状态。后续阶段已加入默认 VST3 清单扫描和 Qt 面板；没有已启用效果器的启动保持旁路，首次勾选的接入行为见 [P7 实现记录](P7_IMPLEMENTATION.md)。

## 已实现

- `native/vst3_autoload.cpp`：Qt `QImageIOPlugin` 自动加载入口。
- `native/modules/`：`vst3_host`、`audio_adapter`、`gp_hook`、`effect_chain`、`qt_ui`、`state_manager` 的最小模块边界。
- `native/host_manifest.json` 和 `native/modules/host_lock.h`：锁定 Guitar Pro 8.1.1.17、Windows x64、Qt 5.15.3，并校验 `GuitarPro.exe`、`GPCore.dll`、`GPRSE.dll`、`AMAudio.dll`、`AMOverloud.dll` 的 SHA-256。
- `native/build.ps1`：使用 Qt 5.15.x MSVC x64 SDK 构建插件 DLL。
- `native/test/host-session.ps1`：复用 GuitarProMCP `start-plugin.ps1` 的进程环境加载方式，直接运行原安装目录的 `GuitarPro.exe`。
- `native/test/test-p0.ps1`：验证原软件的直接启动、指向原软件的临时快捷方式、默认旁路及移除开发加载环境后的正常启动。

状态文件默认位于 `%LOCALAPPDATA%/GuitarProVST3/status.json`，测试时由 `GPVST3_DATA_DIR` 指定。宿主哈希不匹配时保持旁路并记录 `host_unsupported`，不执行 hook。

## 原软件免安装测试（2026-09-10）

所有宿主回归入口（P0、P1/P2、P3/P4/P6 工作流、P7 清单和 MCP）均通过 `host-session.ps1` 启动原软件，不创建 Guitar Pro 程序副本，也不安装、覆盖或移除正式目录中的插件。

- `QT_PLUGIN_PATH` 指向指定开发 DLL 所属的 `plugins` 目录。需要 MCP 时再加入 GuitarProMCP 的开发插件目录，并设置 `QT_QPA_GENERIC_PLUGINS=guitarpro_mcp`。
- 只为新进程设置加载环境。`GPVST3_DATA_DIR`、MCP 数据/会话和 `TEMP`/`TMP` 指向本轮 `artifacts/` 目录；启动后立即恢复调用进程的环境。测试曲谱单独复制，VST3 bundle 和音频设备仍使用本机资源。
- 每个宿主测试核对实际 EXE 路径、PID、状态中的 `plugin_path` 和已加载 DLL；MCP 回归同时核对 MCP DLL 路径及会话 PID，防止误测正式安装的旧版或连接到其他实例。
- `host-integrity.json` 保存原安装目录所有文件的测试前后 SHA-256；文件新增、删除或内容变化都会使测试失败。默认只关闭本轮创建的进程；`-KeepHost` 保留进程，证据始终保留。
- `shutdown.json` 记录进程是否真正结束、是否使用强制清理及退出码；清理通过持有的进程句柄检查终态，避免 `HasExited` 在 DLL 析构尚未完成时提前返回真值。
- P0 移除开发加载环境后，检查开发 DLL 不再加载；若正式目录已有插件，验证它恢复加载。该检查不再宣称完成真实安装/卸载验收。

哈希拒绝测试见 [P6 实现记录](P6_IMPLEMENTATION.md)。安装工具的文件操作测试使用人工生成、不能运行的夹具目录，不复制或修改原软件。此前副本测试的记录只作为历史证据保留。

## 验证

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.3/msvc2019_64
./native/test/test-p0.ps1
```

本轮免安装迁移已验证：原软件直接/快捷方式启动、开发加载环境移除、P1 三个插件的生命周期和处理探针、P2 实时处理、P4 两种输入路由及总旁路、P6 完整回归，以及 P7 默认/显式启用/显式禁用和标准目录扫描。P7 默认启动另以冲突的父进程环境变量验证隔离和恢复；全部 22 个 PowerShell 脚本通过 PowerShell 7.6.5 和 Windows PowerShell 5.1 语法检查。

汇总证据：`artifacts/no-install-migration-20260910/verification.json`，逐项关联 14 轮宿主运行。每轮实际 EXE 均位于原安装目录，开发 DLL 身份已核验，原安装目录 92 个文件的 SHA-256 全部保持一致；核验结束时没有遗留 Guitar Pro 测试进程。完整 P6 证据为 `artifacts/p6-161425c797844e55a39a7f77becc118a/verification.json`。正常退出需等待启动扫描完成，边界见 P6 记录。

当前机器有 Qt 5.15.2 MSVC x64 SDK，可用于 ABI 兼容的本地构建检查；生产锁定仍记录为宿主实际使用的 Qt 5.15.3。真实 Guitar Pro 回归需要匹配的安装目录和桌面会话。
