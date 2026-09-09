# P1 实现记录：DLL 内嵌 VST3 Host

P1 在 P0 自动加载 DLL 内增加了最小 VST3 Host。模块扫描和实例生命周期在独立工作线程执行，音频处理回调仍留到 P2；插件启动时默认旁路。

## 已实现

- `third_party/vst3sdk/` 固定纳入官方 VST3 SDK 3.8.1 源码和许可证。
- `native/build.ps1` 将 SDK 的 VST3 接口 IID、`MemoryStream` 和必要依赖编译进自动加载 DLL；不需要额外进程或运行时。
- `native/modules/vst3_host.cpp` 在非实时线程完成：
  - Windows x64 `.vst3` bundle 扫描和 `GetPluginFactory` 加载。
  - `IPluginFactory3/2` class UID 和 Unicode 元数据枚举。
  - `IComponent` 创建、初始化、默认音频 bus 激活、state chunk 读写回环。
  - `IAudioProcessor` 的 `setupProcessing`、`setActive`、`setProcessing`，以及尾音和延迟读取。
  - 对应 `IEditController` 的初始化、参数数量和 bypass 参数探测、controller state 读写回环。
  - 兼容独立 controller class 和 controller 挂在 `IComponent` 上的 single-component 插件。
  - 每个插件释放前调用 `setProcessing(false)`、`setActive(false)` 和 `terminate()`。
- 默认只探测 `ParametricOD.vst3`、`Gateway.vst3`、`NAM Rig.vst3`；设置 `GPVST3_VST3_PATHS`（分号分隔）或 `GPVST3_VST3_ROOT` 时扫描指定路径，便于开发验证。
- P0 状态文件的 `vst3_host` 字段记录扫描、线程、实例、生命周期、参数、state、尾音和错误信息。

## 验证

```powershell
./native/build.ps1
./native/test-p1.ps1
```

在本机 Guitar Pro 8.1.1.17 隔离副本中，直接启动和快捷方式启动均通过。每次均发现并加载 3 个 bundle，枚举 4 个 class，创建 3 个 audio component，3 个生命周期通过；Gateway 和 ParametricOD 的参数/bypass 回环成功，状态记录 `worker_thread=true`、`ready=true`、state round-trip 成功，且退出后释放模块。证据文件由测试脚本写入被忽略的 `artifacts/` 目录。

## 边界

P1 只验证 Host 生命周期和元数据，不把 GP 音频缓冲接入 VST3，也不在实时线程扫描磁盘、创建实例或动态切换链；这些属于 P2/P3。第三方插件的原生崩溃仍不能由进程内 Host 安全恢复。
