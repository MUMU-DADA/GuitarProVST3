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

正常启动后，在音源区域点击 `VST3`。首次扫描时入口立即显示扫描状态；识别成功的插件自动进入可用列表，识别中、失败和超时的插件不显示为可操作条目。后台逐个识别，超过 10 秒的任务被忽略并推进队列。

在 GP 音轨侧栏的原生音源效果链后显示 `音轨 VST3 效果器`；曲谱侧栏的母带后期处理后显示 `全局 Master VST3 效果器`，两区各有分界线。勾选项进入上方的“正在使用”，从上到下决定实际声音处理顺序，可拖动或用 `Alt+Up` / `Alt+Down` 排序。切换音轨会显示该轨的独立链，全局链保持独立。

音轨增删、重排、保存/另存、关闭重开和进程重启会保留对应链和参数；无需逐轨打开选择区才能恢复。发布 DLL 可独立发现音轨，不需要安装 GuitarProMCP。实际回归范围见 [P8 实现记录](P8_IMPLEMENTATION.md)。

勾选已识别效果器即可启用，点击名称打开独立原生 GUI。关闭 GUI 或选择区后效果继续工作；停用当前插件会关闭其 GUI。无需设置开发环境变量；如曾显式设置 `GPVST3_ENABLE_P2_HOOK=0`，请移除该设置并重启。

默认数据目录为 `%LOCALAPPDATA%/GuitarProVST3`：

- `effect-chain.json` 使用 schema 2 保存 `global.effects` 及按曲谱/音轨 key 分隔的 `scores.*.tracks.*.effects`；旧 schema 1 会自动迁移到 `global.effects`，并保留兼容用的顶层 `effects` 视图。请保留该文件。
- `vst3-catalog-cache.json` 是自动生成的扫描缓存。缓存损坏会自动静态重建；如需手动重建，关闭 GP 后仅删除该缓存，随后重新启动。缓存不随发布包分发。

自动刷新不会反复识别已经超时的同一插件；再次点击 `VST3` 可手动重试，插件文件更新也会触发重新识别。

运行状态、测试证据及宿主边界见 [P8 实现记录](P8_IMPLEMENTATION.md) 和 [P7 实现记录](P7_IMPLEMENTATION.md)。

开发测试使用[原软件免安装入口](P0_IMPLEMENTATION.md#原软件免安装测试2026-09-10)，直接加载仓库构建的 DLL；无需执行上述安装或卸载命令。
