# 测试与验证

测试使用已安装的 Guitar Pro 8.1.1.17 进程加载仓库 DLL，不复制、覆盖或安装测试宿主。测试数据、缓存、临时 DLL 和日志写入被忽略的 `.tools/` 或 `artifacts/`。

## 构建

```powershell
./native/build.ps1 -OutputRoot .tools/native/p8-track-build
```

需要指定 Qt 时使用 `-QtDir C:/path/to/Qt/5.15.x/msvc2019_64`。生产构建不包含测试用的 `-ForceNativeAudioBindings` 覆盖。

## 常用回归

```powershell
./native/test/test-p0.ps1
./native/test/test-p6.ps1 -SkipRuntime
./native/test/test-p8.ps1 -OutputRoot .tools/native/p8-delivery-suite
./native/test/test-p8-recognition-host.ps1
./native/test/test-p8-order.ps1 -P4Route input_insert
./native/test/test-p8-order.ps1 -P4Route bus_mix
./native/test/test-p6-package.ps1 -PluginPath .tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll
git diff --check
```

## 音轨 runtime 回归

先运行 `./native/test/build-p7-gain-fixture.ps1` 生成测试 VST3，再分别验证 MCP bridge 和独立 native collector：

```powershell
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -Vst3Root 'C:/path/to/P7 Gain Fixture.vst3' -ExpectedBindingSource mcp_bridge
./native/build.ps1 -ForceNativeAudioBindings -OutputRoot .tools/native/p8-native-build
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -PluginPath .tools/native/p8-native-build/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root 'C:/path/to/P7 Gain Fixture.vst3' -ExpectedBindingSource native_document_registry
```

## 证据边界

完整宿主回归需要匹配版本的 Guitar Pro、桌面会话和可用音频设备；未运行的设备矩阵或听感项目不能由夹具 PASS 代替。提交前不要将 `artifacts/`、`.tools/`、测试 VST3、缓存或安装包纳入提交。
