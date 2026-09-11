# GuitarProVST3

面向开发者的 Guitar Pro 进程内实时 VST3 效果器链实现。目标宿主为 Guitar Pro 8.1.1.17 / Windows x64；插件由 Guitar Pro 加载，复用 GP 的音频设备和流生命周期。

## 开发环境

- Windows x64
- Guitar Pro 8.1.1.17
- Qt 5.15.x MSVC x64
- PowerShell 5.1 或 PowerShell 7+
- 可选：GuitarProMCP，用于部分真实宿主驱动回归

实时 ABI 受宿主文件哈希和函数 prologue 门控；其他 GP 版本默认旁路。

## 构建

```powershell
./native/build.ps1 -OutputRoot .tools/native/p8-track-build
```

指定 Qt SDK：

```powershell
./native/build.ps1 -QtDir C:/path/to/Qt/5.15.x/msvc2019_64 -OutputRoot .tools/native/p8-track-build
```

`-ForceNativeAudioBindings` 只用于 native collector 测试构建，不用于发布 DLL。

## 验证

常用 PowerShell 回归、音轨 runtime、P4 顺序和发布包检查见 [测试与验证](docs/TESTING.md)。真实宿主回归使用已安装的 Guitar Pro 进程，不复制或改写正式安装目录；输出写入被忽略的 `.tools/` 和 `artifacts/`。

提交前至少运行：

```powershell
git diff --check
```

## 代码与文档入口

- [实时实现总览](docs/REALTIME_IMPLEMENTATION_PLAN.md)：目标、架构、阶段状态和宿主边界。
- [P8 范围与验收](docs/P8_TRACK_GLOBAL_VST3_PLAN.md)：track/global 链、后台识别、UI 和顺序模型。
- [P8 实现记录](docs/P8_IMPLEMENTATION.md)：当前实现、验证摘要和未覆盖范围。
- [测试与验证](docs/TESTING.md)：构建及回归命令。
- [文档归档](docs/archive/README.md)：阶段记录和重整前的完整快照。

## 当前限制

- 真实验证范围锁定 Guitar Pro 8.1.1.17 / Windows x64。
- 不同 ASIO/WASAPI 设备、真实扬声器听感、capture 监听/反馈和其他 GP 版本尚未形成完整矩阵。
- 第三方插件进程内崩溃隔离未实现；factory 超时丢弃迟到结果，但不强制终止第三方线程。

提交时不要包含令牌、客户端配置、临时宿主、缓存、构建产物、安装包、`.tools/` 或 `artifacts/`。
