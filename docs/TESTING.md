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
./native/test/test-p8-runtime.ps1 -PluginPath .tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll -ExternalEditorPlugin 'C:/Program Files/Common Files/VST3/ParametricOD.vst3'
./native/test/test-p6-package.ps1 -PluginPath .tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll
git diff --check
```

## 音轨 runtime 回归

先运行 `./native/test/build-p7-gain-fixture.ps1` 生成测试 VST3，再分别验证 MCP bridge 和独立 native collector：

```powershell
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -Vst3Root 'C:/path/to/P7 Gain Fixture.vst3' -ExpectedBindingSource mcp_context_native_registry
./native/build.ps1 -ForceNativeAudioBindings -OutputRoot .tools/native/p8-native-build
./native/test/test-p8-track-runtime.ps1 -CheckLifecycle -PluginPath .tools/native/p8-native-build/plugins/imageformats/guitarpro_vst3_autoload.dll -Vst3Root 'C:/path/to/P7 Gain Fixture.vst3' -ExpectedBindingSource native_document_registry
```

## 证据边界

完整宿主回归需要匹配版本的 Guitar Pro、桌面会话和可用音频设备；未运行的设备矩阵或听感项目不能由夹具 PASS 代替。提交前不要将 `artifacts/`、`.tools/`、测试 VST3、缓存或安装包纳入提交。

## P9 验证

P9 夹具验证不使用 computer use；宿主流程使用已安装的 MCP bridge/`host-session.ps1`，并将运行数据写入独立 artifacts 目录。

```powershell
./native/test/test-p9-switch.ps1
./native/test/test-p9-ui.ps1
./native/test/test-p9.ps1
```

状态压缩和启动开关使用独立临时目录验证；完整宿主测试中，MCP bridge 的诊断值为 `mcp_context_native_registry`，表示 bridge 提供选中音轨上下文、native registry 提供可处理 EffectsChain。

切换专项记录 UI 请求确认、控制 worker 合并、准备/交接/reader drain 耗时、ramp 和音频块连续性；UI 专项覆盖 About 工具栏按钮幂等、窗口复用、窄侧栏/DPI 和扫描错误不出现在可见文本或 tooltip。`test-p9.ps1` 串联两个专项并保留 evidence。真实 Guitar Pro 听感和设备回归仍须单独记录。
