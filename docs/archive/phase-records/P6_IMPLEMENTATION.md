# P6 实现记录：版本门控、回归和发布

P6 把已验证的 Guitar Pro 8.1.1.17 哈希作为实时 hook 的必要前置条件，并提供回归和发布包流程。当前所有宿主启动均使用[原软件免安装入口](P0_IMPLEMENTATION.md#原软件免安装测试2026-09-10)。哈希不匹配时，插件保持旁路，`gp_hook.installed=false`，VST3 运行时保持未就绪。

## 已实现

- `native/modules/host_lock.h`
  - 集中维护五个宿主文件的 SHA-256 白名单。
  - `verifyDirectory()` 供运行时和测试复用；`verify()` 仍校验当前 `GuitarPro.exe` 目录。
- `native/test/test-p6-gate.ps1`、`native/test/p6_host_lock_test.cpp`
  - 只读原安装目录并验证全部哈希，再依次传入五个文件的错误期望哈希，确认逐项拒绝；空夹具另覆盖文件缺失。
  - `native/test/test-p0.ps1 -RejectHostFile GuitarPro.exe` 在独立输出目录构建负向测试 DLL，仅修改该构建的一个期望哈希，再免安装加载进原软件，确认 hook、VST3 和实时处理被禁用。生产构建没有该覆盖宏，原软件文件不复制、不篡改。
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
  - 在人工生成的文件夹具中验证包清单、哈希、重复安装、篡改后拒绝卸载和正常卸载；其中 `GuitarPro.exe` 只是文本占位文件，不是软件副本，也不会执行。
- `native/test/test-p6.ps1`
  - 统一执行哈希门控、免安装启动/环境移除、输入路由、sidecar、Qt UI、发布包和真实宿主工作流回归。

## 验证

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.2/msvc2019_64
./native/test/test-p6-gate.ps1 -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
./native/test/test-p6-package.ps1
./native/test/test-p6.ps1 -McpRoot C:/path/to/GuitarProMCP
```

`test-p6.ps1 -SkipRuntime` 只跳过 MCP/音频工作流，P0 启动检查仍需要已安装的 Guitar Pro 和桌面会话。完整工作流另需要已构建的 GuitarProMCP 测试插件和音频设备；运行结果写入被忽略的 `artifacts/`。本入口不执行正式安装目录的安装/卸载操作。

## 当前边界

- 当前 `gp_playback` 私有接口只导出 `play/stop`，没有独立 `pause/resume` 操作；P6 证据将 `pause` 标记为 `host_limited`，不会把 `stop` 冒充暂停验收。
- 正常退出工作流先等待启动扫描结束，并沿用 GuitarProMCP 的最长 60 秒退出观察窗口，要求实际进程终态和退出码 0。扫描仍在进行时退出曾在 VST3 DLL 析构中等待；该场景不计正常退出通过，测试清理会记录并终止本轮进程，产品的扫描中退出行为仍待修复。
- GP 对外暴露的设备选择为 `Standard` 和可用时的 `ASIO`；WASAPI/DirectSound 后端及最终设备回调没有独立稳定 ABI。脚本记录可枚举/可切换项，未宣称三种后端听感已完成。
- 真实声卡监听、设备拔插、反馈和最终写回仍受宿主私有 ABI 限制；哈希门控只保证未知版本默认关闭实时 hook。
