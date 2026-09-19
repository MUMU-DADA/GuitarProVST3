param(
    [string]$QtDir = '',
    [string]$OutputRoot = '',
    [string]$Vst3SdkDir = '',
    [string]$GuitarProMcpRoot = '',
    [switch]$ForceNativeAudioBindings,
    [switch]$EnableP13Probe,
    [ValidateSet('', 'GuitarPro.exe', 'GPCore.dll', 'GPRSE.dll', 'AMAudio.dll', 'AMOverloud.dll')]
    [string]$RejectHostFile = '',
    [ValidateRange(0, 5000)]
    [int]$CatalogDelayMs = 0
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $QtDir) {
    $candidates = @(
        (Join-Path $projectRoot '.tools/qt/5.15.3/msvc2019_64'),
        (Join-Path $projectRoot '.tools/qt/5.15.2/msvc2019_64'),
        (Join-Path $env:USERPROFILE 'source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64')
    )
    $QtDir = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $QtDir -or -not (Test-Path -LiteralPath $QtDir)) { throw 'Qt 5.15.x MSVC x64 SDK not found. Pass -QtDir explicitly.' }
$QtDir = (Resolve-Path -LiteralPath $QtDir).Path

if (-not $Vst3SdkDir) { $Vst3SdkDir = Join-Path $projectRoot 'third_party/vst3sdk' }
if (-not (Test-Path -LiteralPath (Join-Path $Vst3SdkDir 'pluginterfaces/base/funknown.h'))) {
    throw 'VST3 SDK not found. Initialize third_party/vst3sdk or pass -Vst3SdkDir explicitly.'
}
$Vst3SdkDir = (Resolve-Path -LiteralPath $Vst3SdkDir).Path

if (($RejectHostFile -or $CatalogDelayMs -or $ForceNativeAudioBindings -or $EnableP13Probe) -and (-not $OutputRoot -or
    [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\', '/') -ieq (Join-Path $projectRoot '.tools/native'))) {
    throw 'A negative-test DLL requires a separate -OutputRoot.'
}
if (-not $OutputRoot) { $OutputRoot = Join-Path $projectRoot '.tools/native' }
$buildDir = Join-Path $OutputRoot 'build'
$pluginDir = Join-Path $OutputRoot 'plugins/imageformats'
New-Item -ItemType Directory -Force -Path $buildDir,$pluginDir | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
foreach ($module in @('gpcore', 'gprse')) {
    & lib /nologo /machine:x64 "/def:$PSScriptRoot/$module-audio.def" "/out:$buildDir/$module.lib"
    if ($LASTEXITCODE) { throw "Audio ABI import library failed: $module" }
}

$includeDirs = @(
    (Join-Path $QtDir 'include'),
    (Join-Path $QtDir 'include/QtCore'),
    (Join-Path $QtDir 'include/QtGui'),
    $Vst3SdkDir,
    (Join-Path $Vst3SdkDir 'public.sdk'),
    (Join-Path $PSScriptRoot 'modules'),
    (Join-Path $PSScriptRoot 'third_party/minhook/include'),
    $buildDir
)
$includeArgs = $includeDirs | ForEach-Object { "-I$_" }
$clIncludeArgs = $includeDirs | ForEach-Object { "/I$_" }
$moc = Join-Path $QtDir 'bin/moc.exe'
& $moc @includeArgs (Join-Path $PSScriptRoot 'vst3_autoload.cpp') -o (Join-Path $buildDir 'vst3_autoload.moc')
if ($LASTEXITCODE) { throw 'Qt moc failed.' }

$sources = @(
    (Join-Path $PSScriptRoot 'vst3_autoload.cpp'),
    (Join-Path $PSScriptRoot 'modules/bootstrap.cpp'),
    (Join-Path $PSScriptRoot 'modules/state_manager.cpp'),
    (Join-Path $PSScriptRoot 'modules/qt_ui.cpp'),
    (Join-Path $PSScriptRoot 'modules/audio_adapter.cpp'),
    (Join-Path $PSScriptRoot 'modules/input_router.cpp'),
    (Join-Path $PSScriptRoot 'modules/effect_chain.cpp'),
    (Join-Path $PSScriptRoot 'modules/gp_audio_runtime.cpp'),
    (Join-Path $PSScriptRoot 'modules/gp_hook.cpp'),
    (Join-Path $PSScriptRoot 'modules/vst3_host.cpp'),
    (Join-Path $PSScriptRoot 'modules/vst3_catalog.cpp'),
    (Join-Path $Vst3SdkDir 'pluginterfaces/base/coreiids.cpp'),
    (Join-Path $Vst3SdkDir 'pluginterfaces/base/funknown.cpp'),
    (Join-Path $Vst3SdkDir 'pluginterfaces/base/ustring.cpp'),
    (Join-Path $Vst3SdkDir 'public.sdk/source/common/memorystream.cpp'),
    (Join-Path $Vst3SdkDir 'public.sdk/source/vst/vstinitiids.cpp')
)
$testDefines = @()
if ($ForceNativeAudioBindings) { $testDefines += '/DGPVST3_FORCE_NATIVE_AUDIO_BINDINGS' }
if ($EnableP13Probe) { $testDefines += '/DGPVST3_P13_PROBE_BUILD' }
$sources += @(
    (Join-Path $PSScriptRoot 'modules/asio_lifecycle_probe.cpp'),
    (Join-Path $PSScriptRoot 'third_party/minhook/src/buffer.c'),
    (Join-Path $PSScriptRoot 'third_party/minhook/src/hook.c'),
    (Join-Path $PSScriptRoot 'third_party/minhook/src/trampoline.c'),
    (Join-Path $PSScriptRoot 'third_party/minhook/src/hde/hde64.c')
)
if ($CatalogDelayMs) { $testDefines += "/DGPVST3_TEST_SCAN_DELAY_MS=$CatalogDelayMs" }
if ($RejectHostFile) {
    $index = @('GUITARPRO.EXE','GPCORE.DLL','GPRSE.DLL','AMAUDIO.DLL','AMOVERLOUD.DLL').IndexOf($RejectHostFile.ToUpperInvariant())
    $testDefines += "/DGPVST3_TEST_REJECT_HOST_INDEX=$index"
}
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /LD /DQT_NO_DEBUG /DQT_PLUGIN /DUNICODE /D_UNICODE @testDefines @clIncludeArgs @sources "/Fo$buildDir/" "/Fd$buildDir/guitarpro_vst3_autoload.pdb" "/Fe$pluginDir/guitarpro_vst3_autoload.dll" /link /Brepro "/LIBPATH:$QtDir/lib" "/LIBPATH:$buildDir" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib Ole32.lib User32.lib GPCore.lib GPRSE.lib "/IMPLIB:$buildDir/guitarpro_vst3_autoload.lib"
if ($LASTEXITCODE) { throw 'P0 plugin compilation failed.' }
Write-Output "Built $pluginDir/guitarpro_vst3_autoload.dll"
