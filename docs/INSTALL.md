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
