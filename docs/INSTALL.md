# GuitarProVST3 安装

1. 关闭 Guitar Pro。
2. 解压发布 ZIP。
3. 在 PowerShell 中运行：

```powershell
./install.ps1 -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

插件会安装到 Guitar Pro 的 `Plugins/imageformats`，并写入文件归属收据。只有收据哈希仍与 DLL 相符时，再次安装或卸载才会继续。

卸载：

```powershell
./uninstall.ps1 -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

实时 hook 只在 `config/host_manifest.json` 对应的 Guitar Pro 8.1.1.17 / Windows x64 文件全部匹配时启用；其他版本保持旁路。

正常启动后，在音源区域点击 `VST3`。首次扫描时入口立即显示扫描状态；识别结果和“待识别”候选项会自动出现。勾选待识别项只加载该插件进行识别，多个效果器 class 会列出供选择；识别失败时保持未启用并显示原因，再次勾选才会重试。扫描和刷新仅读取本地文件，主动启用或恢复插件后，其代码仍可能自行联网。

勾选已识别效果器即可启用，点击名称打开独立原生 GUI。关闭 GUI 或选择区后效果继续工作；停用当前插件会关闭其 GUI。无需设置开发环境变量；如曾显式设置 `GPVST3_ENABLE_P2_HOOK=0`，请移除该设置并重启。

默认数据目录为 `%LOCALAPPDATA%/GuitarProVST3`：

- `effect-chain.json` 保存启用状态和插件参数，请保留。
- `vst3-catalog-cache.json` 是自动生成的扫描缓存。缓存损坏会自动静态重建；如需手动重建，关闭 GP 后仅删除该缓存，随后重新启动。缓存不随发布包分发。

运行状态、测试证据及宿主边界见 [P7 实现记录](P7_IMPLEMENTATION.md)。

开发测试使用[原软件免安装入口](P0_IMPLEMENTATION.md#原软件免安装测试2026-09-10)，直接加载仓库构建的 DLL；无需执行上述安装或卸载命令。
