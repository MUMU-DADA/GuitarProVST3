param(
    [string]$QtDir = '', [string]$OutputRoot = '',
    [string]$PluginPath = 'C:/Program Files/Common Files/VST3/Neural DSP/Archetype Mateus Asato.vst3',
    [int]$SampleRate = 192000, [int]$Frames = 64, [double]$Seconds = 3
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p13-neural-runtime-test' }
if (-not (Test-Path -LiteralPath $PluginPath)) { throw "Installed VST3 is required: $PluginPath" }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$sdk = Join-Path $root 'third_party/vst3sdk'
$sources = @((Join-Path $PSScriptRoot 'p13_neural_runtime_test.cpp')) + @('audio_adapter','effect_chain','input_router','state_manager' | ForEach-Object { Join-Path $root "native/modules/$_.cpp" }) +
    @("$sdk/pluginterfaces/base/coreiids.cpp", "$sdk/pluginterfaces/base/funknown.cpp", "$sdk/pluginterfaces/base/ustring.cpp", "$sdk/public.sdk/source/common/memorystream.cpp", "$sdk/public.sdk/source/vst/vstinitiids.cpp")
$sources += @('buffer.c','hook.c','trampoline.c','hde/hde64.c' | ForEach-Object { Join-Path $root "native/third_party/minhook/src/$_" })
$exe = Join-Path $OutputRoot 'p13_neural_runtime_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/I$QtDir/include" "/I$QtDir/include/QtCore" "/I$sdk" @sources "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib Ole32.lib User32.lib
if ($LASTEXITCODE) { throw 'Real plugin timing fixture build failed.' }
$previousPath = $env:PATH
$previousPlatform = $env:QT_QPA_PLATFORM
try {
    $env:PATH = "$QtDir/bin;$env:PATH"
    $env:QT_QPA_PLATFORM = 'offscreen'
    & $exe $PluginPath (Join-Path $OutputRoot "timing-$SampleRate-$Frames.json") $SampleRate $Frames $Seconds
    if ($LASTEXITCODE) { throw 'Real plugin timing/finite output verification failed.' }
} finally {
    $env:PATH = $previousPath
    $env:QT_QPA_PLATFORM = $previousPlatform
}
