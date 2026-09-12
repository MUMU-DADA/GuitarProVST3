# GuitarProVST3 安装与使用

## 安装

1. 关闭 Guitar Pro。
2. 解压发布 ZIP。
3. 双击 `Install.cmd`，脚本会按 GuitarProMCP 的方式请求管理员权限。也可以在 PowerShell 中运行：

```powershell
./install.ps1 -Elevate -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

插件安装到 Guitar Pro 的 `Plugins/imageformats`，并写入文件归属收据。已有本安装器收据的版本可直接再次运行 `Install.cmd` 更新；只有收据哈希仍与 DLL 相符时，更新或卸载才会继续。

如果安装目录已有旧的 `guitarpro_vst3_autoload.dll` 但没有 GuitarProVST3 收据，双击 `Install.cmd` 会先备份旧文件再完成安装。PowerShell 中可显式启用同样的迁移：

```powershell
./install.ps1 -Elevate -MigrateExisting -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

迁移会先把旧文件备份到 `Plugins/guitarpro-vst3-backups`，再安装当前版本。

更新已有安装：

```powershell
./install.ps1 -Elevate -Action Update -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

卸载：

```powershell
./uninstall.ps1 -HostDirectory 'C:/Program Files/Arobas Music/Guitar Pro 8'
```

也可以双击 `Uninstall.cmd`。

## 使用

实时 hook 只在 `config/host_manifest.json` 对应的 Guitar Pro 8.1.1.17 / Windows x64 文件全部匹配时启用；其他版本保持旁路。

在音源区域点击 `VST3`。识别成功的插件进入可用列表；识别中、失败和超时的插件不显示为可操作条目。后台识别超过 10 秒会忽略当前任务并继续队列。

标题工具栏只保留一个“关于”入口；窗口显示版本、已验证宿主范围、许可证、第三方声明和诊断数据目录，并提供“打开配置”和“启动时启用插件”开关。关闭开关后，插件下次启动不会扫描、加载或接入音频；重新打开开关后重启 Guitar Pro 生效。

音轨 VST3 位于 GP 音轨侧栏的原生音源效果链后；全局 Master VST3 位于曲谱侧栏的母带后期处理后。每次启动都会先停用并旁路所有 VST3 处理器，已保存的链仍会显示在列表中，必须再次勾选才会创建并启用实例。勾选项进入“正在使用”，从上到下决定处理顺序，可拖动或使用 `Alt+Up` / `Alt+Down` 排序。切换音轨只改变该轨链，全局链保持独立。

关闭 GUI 或选择区后效果继续工作；勾选切换会先显示“请求中”，后台准备完成后生效，准备期间界面仍可操作。吉他输入经过所选全局 VST3 链处理，全局 GUI 的参数编辑同步到输入效果，音轨参数保持独立。双击插件名称打开 GUI，停用插件会关闭其 GUI。音轨增删、重排、保存/另存和关闭重开会保留对应链和参数，进程重启后仍需显式重新勾选。

## 数据目录

默认目录为 `%LOCALAPPDATA%/GuitarProVST3`：

- `effect-chain.json`：schema 2 的 global/track 链、参数和状态。只持久化启用、已配置、带错误或带插件状态的数据；旧版本写入的空音轨和完整清单会在读取大文件时自动压缩。旧 schema 1 会自动迁移到 `global.effects`，请保留该文件。
- `settings.json`：插件启动开关。缺失或无效时默认启用。
- `vst3-catalog-cache.json`：自动生成的扫描缓存。缓存损坏会自动重建；需要手动重建时，关闭 GP 后只删除该文件并重新启动。

自动刷新不会反复识别已超时的同一插件；再次点击 `VST3`、插件文件更新或扫描器版本变化会触发重试。

开发构建和宿主回归见 [测试与验证](TESTING.md)。
