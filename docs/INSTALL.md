# GuitarProVST3 安装与使用

## 安装

1. 关闭 Guitar Pro。
2. 解压发布 ZIP。
3. 在 PowerShell 中运行：

```powershell
./install.ps1 -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

插件安装到 Guitar Pro 的 `Plugins/imageformats`，并写入文件归属收据。只有收据哈希仍与 DLL 相符时，重复安装或卸载才会继续。

卸载：

```powershell
./uninstall.ps1 -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

## 使用

实时 hook 只在 `config/host_manifest.json` 对应的 Guitar Pro 8.1.1.17 / Windows x64 文件全部匹配时启用；其他版本保持旁路。

在音源区域点击 `VST3`。识别成功的插件进入可用列表；识别中、失败和超时的插件不显示为可操作条目。后台识别超过 10 秒会忽略当前任务并继续队列。

音轨 VST3 位于 GP 音轨侧栏的原生音源效果链后；全局 Master VST3 位于曲谱侧栏的母带后期处理后。勾选项进入“正在使用”，从上到下决定处理顺序，可拖动或使用 `Alt+Up` / `Alt+Down` 排序。切换音轨只改变该轨链，全局链保持独立。

关闭 GUI 或选择区后效果继续工作；停用插件会关闭其 GUI。音轨增删、重排、保存/另存、关闭重开和进程重启会恢复对应链和参数。

## 数据目录

默认目录为 `%LOCALAPPDATA%/GuitarProVST3`：

- `effect-chain.json`：schema 2 的 global/track 链、参数和状态。旧 schema 1 会自动迁移到 `global.effects`，请保留该文件。
- `vst3-catalog-cache.json`：自动生成的扫描缓存。缓存损坏会自动重建；需要手动重建时，关闭 GP 后只删除该文件并重新启动。

自动刷新不会反复识别已超时的同一插件；再次点击 `VST3`、插件文件更新或扫描器版本变化会触发重试。

开发构建和宿主回归见 [测试与验证](TESTING.md)。
