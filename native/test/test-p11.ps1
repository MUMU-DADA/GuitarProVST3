param(
    [string]$QtDir = '',
    [string]$OutputRoot = '',
    [string]$PluginPath = '',
    [switch]$RunHost,
    [switch]$CheckLifecycle,
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$Vst3Root = 'ParametricOD.vst3;Gateway.vst3'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p11-delivery-suite' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

# Static gates protect the P11 contract from a later reintroduction of the
# high-frequency maintenance paths. They run without a host or GUI.
$sources = @(
    (Join-Path $root 'native/modules/bootstrap.cpp'),
    (Join-Path $root 'native/modules/qt_ui.cpp'),
    (Join-Path $root 'native/vst3_autoload.cpp'),
    (Join-Path $root 'native/modules/gp_hook.cpp'),
    (Join-Path $root 'native/modules/gp_audio_runtime.cpp'))
$joined = ($sources | ForEach-Object { Get-Content -LiteralPath $_ -Raw }) -join "`n"
if ($joined -match 'setInterval\(250\)' -or $joined -match 'setInterval\(500\)' -or
    $joined -match 'singleShot\(250' -or $joined -match 'musician->updateAll\(') {
    throw 'P11 static gate found a removed high-frequency maintenance path.'
}
$snapshot = Get-Content -LiteralPath (Join-Path $root 'native/modules/gp_hook.cpp') -Raw
$snapshotStart = $snapshot.IndexOf('State snapshot() noexcept')
$snapshotBody = if ($snapshotStart -ge 0) {
    $tail = $snapshot.Substring($snapshotStart)
    $end = [regex]::Match($tail, '(?m)^}').Index
    $tail.Substring(0, $end + 1)
} else { '' }
if ($snapshotBody -match 'updateAudioLayerState\(\)|reconfigureInputRouterIfNeeded\(\)') {
    throw 'P11 static gate found a snapshot side effect.'
}
$audioRuntime = Get-Content -LiteralPath (Join-Path $root 'native/modules/gp_audio_runtime.cpp') -Raw
$selectionStart = $audioRuntime.IndexOf('bool refreshSelectionContext() noexcept')
$selectionBody = if ($selectionStart -ge 0) {
    $tail = $audioRuntime.Substring($selectionStart)
    $end = [regex]::Match($tail, '(?m)^}').Index
    $tail.Substring(0, $end + 1)
} else { '' }
if ($selectionBody -match 'g_dirty\.store\(false') {
    throw 'P11 static gate found selection-only refresh clearing structure dirtiness.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not (Test-Path -LiteralPath $QtDir)) { throw 'Qt 5.15.x MSVC x64 SDK not found. Pass -QtDir explicitly.' }
$include = @("/I$(Join-Path $QtDir 'include')", "/I$(Join-Path $QtDir 'include/QtCore')",
    "/I$(Join-Path $root 'native/modules')")
$maintenanceExe = Join-Path $OutputRoot 'p11_maintenance_test.exe'
$compileSources = @((Join-Path $root 'native/modules/state_manager.cpp'),
    (Join-Path $PSScriptRoot 'p11_maintenance_test.cpp'))
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @include @compileSources "/Fo$OutputRoot/" "/Fe$maintenanceExe" `
    /link "/LIBPATH:$(Join-Path $QtDir 'lib')" Qt5Core.lib
if ($LASTEXITCODE) { throw 'P11 maintenance fixture compilation failed.' }
$env:PATH = "$(Join-Path $QtDir 'bin');$env:PATH"
& $maintenanceExe
if ($LASTEXITCODE) { throw 'P11 maintenance fixture failed.' }

# Compile the P11-named UI fixture as its own gate. It intentionally reuses
# the established P9 assertions, but compiling it here prevents the new
# scheduling entry from silently becoming an untested source file.
$uiInclude = @("/I$(Join-Path $QtDir 'include')", "/I$(Join-Path $QtDir 'include/QtCore')",
    "/I$(Join-Path $QtDir 'include/QtGui')", "/I$(Join-Path $QtDir 'include/QtWidgets')",
    "/I$(Join-Path $root 'native/modules')")
$uiExe = Join-Path $OutputRoot 'p11_ui_performance_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @uiInclude @((Join-Path $root 'native/modules/state_manager.cpp'),
    (Join-Path $root 'native/modules/qt_ui.cpp'), (Join-Path $PSScriptRoot 'p11_ui_performance_test.cpp')) "/Fo$OutputRoot/" "/Fe$uiExe" `
    /link "/LIBPATH:$(Join-Path $QtDir 'lib')" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib
if ($LASTEXITCODE) { throw 'P11 UI fixture compilation failed.' }
& $uiExe
if ($LASTEXITCODE) { throw 'P11 UI fixture failed.' }

& (Join-Path $PSScriptRoot 'test-p10-activation.ps1') -OutputRoot (Join-Path $OutputRoot 'activation')
& (Join-Path $PSScriptRoot 'test-p9-ui.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'ui')

if ($RunHost) {
    if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p11-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
    $hostArgs = @{HostDirectory=$HostDirectory;PluginPath=$PluginPath;McpRoot=$McpRoot;Vst3Root=$Vst3Root;HookMode='enabled'}
    if ($CheckLifecycle) { $hostArgs.CheckLifecycle = $true }
    & (Join-Path $PSScriptRoot 'test-p8-track-runtime.ps1') @hostArgs
}
@{schema=1;static_gate='pass';maintenance='pass';ui_fixture='pass';activation='pass';ui='pass';host_validation=([bool]$RunHost);lifecycle=([bool]$CheckLifecycle)} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P11 scheduling, maintenance writer, UI and activation regression suite. Evidence: $OutputRoot"
