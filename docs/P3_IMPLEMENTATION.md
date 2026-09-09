# P3 实现记录：实时线程安全和链管理

P3 在 P2 的哈希门控实时入口上增加了固定双槽效果器链。插件实例和 scratch 缓冲在工作线程准备；音频线程只读取原子状态、调用已发布的 processor 回调并更新原子计数器。

## 已实现

- `native/modules/effect_chain.h/.cpp`
  - 两个预分配 slot，使用 `activeSlot` 原子发布链切换。
  - slot 替换前停止接收新读者并等待已有读者退出；音频线程不获取互斥锁、不分配内存、不扫描磁盘、不创建 Qt 对象。
  - `setBypassed()` 提供总旁路；旁路和 slot 竞争时通过 `audio::bypass()` 直接写回 GP 缓冲。
  - processor 返回失败后记录 error/fallback 计数并保持总旁路，避免继续调用已失败实例。
  - 记录最后一次、最大值和累计 `process()` 纳秒耗时。
- `native/modules/gp_hook.cpp`
  - 每个运行时效果器实例拥有自己的 processor、scratch 和 processing guard；工作线程预创建两个实例并在两个 slot 间切换。
  - 在 `44.1/48/96 kHz × 64/128/256 frame` 矩阵中执行 `setupProcessing()`、scratch 重配和原子链切换，并恢复到 44.1 kHz / 16384 frame。
  - 支持 `GPVST3_TOTAL_BYPASS=1` 的总旁路验证和 `GPVST3_FORCE_P3_ERROR=1` 的可检测错误回退验证开关。
- `native/test-p3.ps1`
  - 复用 P2 的真实 Guitar Pro 播放夹具，分别验证正常链处理/重配置和 processor 错误后的自动旁路回退。
- `status.json` / `p2-observation.json`
  - 输出 slot 数、切换次数、处理计时、错误/回退计数、总旁路状态和重配置矩阵结果。

## 验证

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.2/msvc2019_64
./native/test-p3.ps1
```

最近一次锁定的 Guitar Pro 8.1.1.17 / Windows x64 隔离宿主验证已通过：

- 正常链：49 个播放块均处理，`chain_prepared_slots=2`、`chain_error_blocks=0`、`reconfiguration_passed=10`、`reconfiguration_failed=0`、`reconfiguration_validated=true`，处理耗时监控记录到约 2 ms 的最大值（运行时具体值以证据文件为准）。
- 可检测错误回退：`chain_faulted=true`、`total_bypass=true`、`chain_error_blocks=1`、`chain_fallback_blocks=1`，后续块均旁路。
- 显式总旁路：`total_bypass=true`、`chain_faulted=false`、49 个块旁路、处理块为 0。

三次运行均使用 `native/test-p3.ps1`，证据写入被忽略的 `artifacts/` 目录。

## 边界

- 进程内第三方 VST3 的原生崩溃仍不能安全恢复；P3 只处理 `process()` 返回失败、容量不足、空缓冲和忙冲突等可检测错误。
- 任意 `process()` 调用不能被安全强制中断，因此耗时监控只记录证据，不在音频线程执行超时杀断。
- 真实 Guitar Pro 输出回调已在锁定宿主版本中观测到最终缓冲写回；外部输入仍属于宿主受限项。P5 已提供独立 Qt 链编辑和 sidecar 配置，运行时多实例链重建仍未宣称完成。
