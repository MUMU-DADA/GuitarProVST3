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

正常启动后，在音源区域点击 `VST3`，勾选插件即可启用，点击名称打开其 GUI。无需设置开发环境变量；如曾显式设置 `GPVST3_ENABLE_P2_HOOK=0`，请移除该设置并重启后再启用插件。默认启动及扫描的验证边界见 [P7 实现记录](P7_IMPLEMENTATION.md)。

开发测试使用[原软件免安装入口](P0_IMPLEMENTATION.md#原软件免安装测试2026-09-10)，直接加载仓库构建的 DLL；无需执行上述安装或卸载命令。
