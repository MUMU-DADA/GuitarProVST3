# P6 实现记录：版本门控、回归和发布

P6 把已验证的 Guitar Pro 8.1.1.17 哈希作为实时 hook 的必要前置条件，并补齐了可重复的隔离回归和发布包流程。宿主文件任一字节变化时，插件保持旁路，`gp_hook.installed=false`，VST3 运行时保持未就绪；这条路径由正向和篡改副本测试覆盖。

## 已实现

- `native/modules/host_lock.h`
  - 集中维护五个宿主文件的 SHA-256 白名单。
  - `verifyDirectory()` 供运行时和测试复用；`verify()` 仍校验当前 `GuitarPro.exe` 目录。
- `native/test/test-p6-gate.ps1`、`native/test/p6_host_lock_test.cpp`
  - 复制真实宿主文件后验证全部哈希通过，再向副本 `GuitarPro.exe` 追加字节并确认校验失败。
  - `native/test/test-p0.ps1 -TamperHostFile GuitarPro.exe` 在可加载的隔离宿主中确认版本变化自动禁用 hook、VST3 和实时处理。
- `native/test/p6_workflow.ps1` 与 `native/test/test-p2-runtime.ps1 -P6Workflow`
  - 覆盖播放/停止、循环范围与循环回绕、换曲谱、保存/关闭/重开、原生设备状态和清洁退出。
  - 记录 `audioDevice`、`audioOutput`、`audioOutputChannels`、`audioBuffersSize` 的可用选项和实际切换结果；采样率由 GP 私有链路观察字段记录。
- `native/package.ps1`
  - 从指定 DLL 生成版本化 ZIP，包含 DLL、Qt 插件元数据、宿主哈希清单、配置模板、计划/P6 文档和安装工具。
  - `package.json` 为每个文件生成 SHA-256；禁止构建产物、缓存和 `third_party` 文件进入发布目录。
- `native/install.ps1`、`native/uninstall.ps1`
  - 只在目标没有同名未归属 DLL 时安装，并写入 `Plugins/guitarpro-vst3-install.json` 归属收据。
  - 再次安装或卸载前检查当前 DLL 是否仍与收据哈希一致；被修改的文件拒绝覆盖或删除，不触碰 Guitar Pro 主程序和用户数据。
- `native/test/test-p6-package.ps1`
  - 在临时宿主目录验证包清单、哈希、重复安装、篡改后拒绝卸载和正常卸载。
- `native/test/test-p6.ps1`
  - 统一执行哈希门控、启动/卸载、输入路由、sidecar、Qt UI、发布包和真实宿主工作流回归。

## 验证

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.2/msvc2019_64
./native/test/test-p6-gate.ps1 -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
./native/test/test-p6-package.ps1
./native/test/test-p6.ps1 -McpRoot C:/path/to/GuitarProMCP
```

`test-p6.ps1 -SkipRuntime` 仍会执行全部隔离检查，适用于没有桌面 Guitar Pro/MCP 会话的构建机。真实工作流需要 Guitar Pro 8.1.1.17、已构建的 GuitarProMCP 测试插件和桌面音频设备；运行结果写入被忽略的 `artifacts/`。

## 当前边界

- 当前 `gp_playback` 私有接口只导出 `play/stop`，没有独立 `pause/resume` 操作；P6 证据将 `pause` 标记为 `host_limited`，不会把 `stop` 冒充暂停验收。
- GP 对外暴露的设备选择为 `Standard` 和可用时的 `ASIO`；WASAPI/DirectSound 后端及最终设备回调没有独立稳定 ABI。脚本记录可枚举/可切换项，未宣称三种后端听感已完成。
- 真实声卡监听、设备拔插、反馈和最终写回仍受宿主私有 ABI 限制；哈希门控只保证未知版本默认关闭实时 hook。
