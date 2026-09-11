# P4 capture 回调契约与证据边界

## 当前实现

`native/modules/gp_hook.cpp` 在已通过 P6 宿主哈希和 `streamCallback` prologue 门控后，读取 `AMAudio.dll` 8.1.1.17 的当前 PortAudio 参数快照。`native/modules/portaudio_capture_abi.h` 固定记录了本版本观察到的参数布局：输入/输出均为交错 `paFloat32`，通道数分别为 1 或 2，采样率来自当前 GP 音频核心。回调指针只在本次调用期间借用；输入先转为 planar float32，路由完成后立即写回输出交错缓冲，不保存任何宿主指针。

回调中只有以下条件全部满足时才启用输入路由：参数快照有效、输入和输出指针非空、帧数在预分配容量内、输入功能已显式启用且路由为 `input_insert` 或 `bus_mix`。条件不满足时保留 GP 原始输出，并在状态快照中记录 `input_configuration_errors` 或 `input_interleaved_missing_blocks`。

单声道 capture 会复制到处理链的每个输出通道；`input_insert` 只处理外部输入，`bus_mix` 先把回调输出作为 GP 生成流与 capture 相加。处理器失败时保留直通回退。总旁路和停止状态均不会把效果器输出写入设备缓冲。

## 已验证

- `native/test/p4_input_router_test.cpp` 验证 mono capture → stereo output 的交错转换、写回、借用 owner 记录和空输入安全回退。
- `native/test/test-p4-router.ps1` 验证路由、峰值/RMS、计数器和边界路径；`native/build.ps1` 验证插件 DLL 编译。
- `./native/test/test-p4.ps1` 已在 Guitar Pro 8.1.1.17 锁定宿主上通过（2026-09-10），并分别回归 `input_insert` 与 `bus_mix`。本次两次快照均观察到 `input_interleaved_observed=true`、`input_interleaved_output_written=true`、输入/输出均为 2 通道、采样率为 44100 Hz，`input_configuration_errors=0`；`input_insert` 处理 157 个 capture block，`bus_mix` 混音 156 个 block，capture/output 地址和 `userData` owner witness 均非零。

## 未宣称完成

本项目没有把 PortAudio 的私有 `userData` 对象布局当作稳定 ABI，也没有在未知宿主版本上猜测输入通道数。真实回归只在哈希锁定的宿主版本和默认音频配置下执行；capture 指针所有权仍是回调期借用，未保存跨回调指针。真实监听稳定性、声学反馈、设备切换、暂停/恢复以及不同 ASIO/WASAPI 设备的听感仍需实际设备验证，状态继续标记为宿主受限。
