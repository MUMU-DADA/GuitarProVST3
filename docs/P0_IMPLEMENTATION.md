# P0 实现记录

P0 只建立插件自动加载入口、宿主版本锁定和默认旁路状态。未设置后续阶段的显式开关时，插件不扫描 VST3、不修改 GP 音频缓冲、不安装私有 ABI hook；P5 的 Qt 面板在后续阶段加入并保持独立旁路。

## 已实现

- `native/vst3_autoload.cpp`：Qt `QImageIOPlugin` 自动加载入口。
- `native/modules/`：`vst3_host`、`audio_adapter`、`gp_hook`、`effect_chain`、`qt_ui`、`state_manager` 的最小模块边界。
- `native/host_manifest.json` 和 `native/modules/host_lock.h`：锁定 Guitar Pro 8.1.1.17、Windows x64、Qt 5.15.3，并校验 `GuitarPro.exe`、`GPCore.dll`、`GPRSE.dll`、`AMAudio.dll`、`AMOverloud.dll` 的 SHA-256。
- `native/build.ps1`：使用 Qt 5.15.x MSVC x64 SDK 构建插件 DLL。
- `native/test/test-p0.ps1`：在隔离宿主副本中验证直接启动、快捷方式启动、默认旁路和移除恢复。

插件加载后只写入状态文件（默认位于 `%LOCALAPPDATA%/GuitarProVST3/status.json`，测试时由 `GPVST3_DATA_DIR` 指定），默认保持 `bypassed=true`。宿主哈希不匹配时仍保持旁路并记录 `host_unsupported`，不执行 hook。

## 验证

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.3/msvc2019_64
./native/test/test-p0.ps1
```

当前机器有 Qt 5.15.2 MSVC x64 SDK，可用于 ABI 兼容的本地构建检查；生产锁定仍记录为宿主实际使用的 Qt 5.15.3。真实 Guitar Pro 回归需要匹配的安装目录和桌面会话。
