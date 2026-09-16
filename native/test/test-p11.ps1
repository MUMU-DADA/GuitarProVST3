param([string]$QtDir = '', [string]$OutputRoot = '', [string]$PluginPath = '', [switch]$RunHost, [switch]$CheckLifecycle,
      [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
      [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP', [string]$Vst3Root = 'ParametricOD.vst3;Gateway.vst3')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p11-suite' }
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$sources = @((Join-Path $root 'native/modules/state_manager.cpp'), (Join-Path $PSScriptRoot 'p11_maintenance_test.cpp'))
$include = @("/I$QtDir/include", "/I$QtDir/include/QtCore", "/I$root/native/modules")
$exe = Join-Path $OutputRoot 'p11_maintenance_test.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' }
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @include @sources "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib
if ($LASTEXITCODE) { throw 'P11 maintenance fixture compilation failed.' }
$env:PATH = "$QtDir/bin;$env:PATH"; & $exe
if ($LASTEXITCODE) { throw 'P11 maintenance fixture failed.' }
$uiExe = Join-Path $OutputRoot 'p11_ui_performance_test.exe'
$uiInclude = @("/I$QtDir/include", "/I$QtDir/include/QtCore", "/I$QtDir/include/QtGui", "/I$QtDir/include/QtWidgets", "/I$root/native/modules")
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @uiInclude @((Join-Path $root 'native/modules/state_manager.cpp'), (Join-Path $root 'native/modules/qt_ui.cpp'), (Join-Path $PSScriptRoot 'p11_ui_performance_test.cpp')) "/Fo$OutputRoot/" "/Fe$uiExe" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib
if ($LASTEXITCODE) { throw 'P11 UI fixture compilation failed.' }
& $uiExe
if ($LASTEXITCODE) { throw 'P11 UI fixture failed.' }
$joined = (@('native/modules/bootstrap.cpp','native/modules/qt_ui.cpp','native/vst3_autoload.cpp','native/modules/gp_hook.cpp','native/modules/gp_audio_runtime.cpp') | ForEach-Object { Get-Content (Join-Path $root $_) -Raw }) -join "`n"
$p11Static = $joined -replace 'g_trackFallbackTimer->setInterval\(250\)', ''
if ($p11Static -match 'setInterval\(250\)' -or $p11Static -match 'setInterval\(500\)' -or $p11Static -match 'singleShot\(250' -or $p11Static -match 'musician->updateAll\(') { throw 'P11 static gate failed.' }
$hook = Get-Content (Join-Path $root 'native/modules/gp_hook.cpp') -Raw
$snapshotBody = [regex]::Match($hook, '(?s)State snapshot\(\) noexcept\s*\{.*?\n\}').Value
if ($snapshotBody -match 'updateAudioLayerState\(\)|reconfigureInputRouterIfNeeded\(\)') { throw 'P11 snapshot side effect gate failed.' }
& (Join-Path $PSScriptRoot 'test-p10-activation.ps1') -OutputRoot (Join-Path $OutputRoot 'activation')
& (Join-Path $PSScriptRoot 'test-p9-ui.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'ui')
if ($RunHost) {
  if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p11-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
  $args = @{HostDirectory=$HostDirectory;PluginPath=$PluginPath;McpRoot=$McpRoot;Vst3Root=$Vst3Root;HookMode='enabled'}
  if ($CheckLifecycle) { $args.CheckLifecycle = $true }
  & (Join-Path $PSScriptRoot 'test-p8-track-runtime.ps1') @args
}
@{schema=1;static_gate='pass';maintenance='pass';ui_fixture='pass';activation='pass';ui='pass';host_validation=[bool]$RunHost} | ConvertTo-Json | Set-Content (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P11 scheduling, maintenance, UI and activation suite. Evidence: $OutputRoot"
