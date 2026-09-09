# P4 实现记录：外部吉他输入

P4 已完成最小可交付的输入适配层和隔离验证。插件不创建第二套声卡流，capture tap 只接收 `AudioLayer`/PortAudio 提供的当前块指针；处理完成后立即写回宿主提供的输出缓冲。

## 已实现

- `native/modules/input_router.h/.cpp`
  - `CaptureView`、`GeneratedView` 和 `OutputView` 描述 capture、GP 回放和输出缓冲。
  - 控制线程预分配固定容量 scratch；capture 处理路径不分配、不访问 Qt、不扫描磁盘、不等待阻塞锁。
  - `Route::InputInsert`：外部输入单独进入 VST3 processor，输出只包含处理后的外部输入。
  - `Route::BusMix`：外部输入与 GP 生成音频逐样本相加后进入总线 processor。
  - `Route::Disabled`：保留 GP 生成音频（capture-only 调用则直通 capture）。processor 失败时直通并累计错误/旁路计数。
  - 每个 capture 块记录峰值、RMS、采样率、帧数、通道数和处理计数，全部通过原子快照提供给非实时线程。
- `native/modules/gp_hook.cpp`
  - 解析已验证 `AMAudio.dll` 的 `AudioLayer::instance`、`inputLevel`、`isRunning` 和 `bufferSize` 导出，读取宿主输入电平和流状态。
  - 新增 `processExternalInput()` 作为 PortAudio/`AudioLayerWorker` 的唯一 capture 接入契约。调用该契约后才把 `input_capture_path_located` 标记为 `true`。
  - `GPVST3_ENABLE_P4_INPUT=1` 配合 `GPVST3_P4_ROUTE=input_insert|bus_mix` 时，在工作线程创建独立的输入 processor；默认仍关闭。
- `native/modules/bootstrap.cpp` 和状态快照新增 P4 路由、输入电平、流状态、capture 计数及错误字段。
- `native/test-p4-router.ps1` 与 `native/tests/p4_input_router_test.cpp`
  - 覆盖输入插入、GP+输入混音、旁路直通、峰值/RMS 和计数器。

## 验证

```powershell
./native/build.ps1
./native/test-p4-router.ps1
```

本次已通过 MSVC x64 插件构建和独立路由夹具。夹具确认三种路由的样本结果、输入电平统计和错误安全回退均符合预期。

在真实 Guitar Pro 8.1.1.17 中，`AMAudio` 导出的 `AudioLayer::inputLevel`/流状态访问器可用于监控；预研中发现的 `PortAudioAudioLayerImpl::Impl::streamCallback` 只存在于字符串和调用证据，没有稳定导出或可校验的函数 prologue。因此真实 capture 指针尚未宣称已定位，必须由后续宿主适配器在确认该回调 ABI 后调用 `processExternalInput()`。真实输入监听、反馈、设备切换和暂停/恢复听感仍属于宿主受限验证项。

## 运行开关

- `GPVST3_ENABLE_P4_INPUT=1`：启用 P4 输入 processor 创建。
- `GPVST3_P4_ROUTE=input_insert`：外部输入单独处理。
- `GPVST3_P4_ROUTE=bus_mix`：外部输入与 GP 回放混合后处理。
- 未设置上述开关时，P4 路由保持 `disabled`，不会改变 P0–P3 默认行为。
