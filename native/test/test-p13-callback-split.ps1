param([string]$QtDir = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p13-callback-split-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$sdk = Join-Path $root 'third_party/vst3sdk'
$sources = @((Join-Path $PSScriptRoot 'p13_callback_split_test.cpp')) + @('audio_adapter','effect_chain','input_router','state_manager' | ForEach-Object { Join-Path $root "native/modules/$_.cpp" }) +
    @("$sdk/pluginterfaces/base/coreiids.cpp", "$sdk/pluginterfaces/base/funknown.cpp", "$sdk/pluginterfaces/base/ustring.cpp", "$sdk/public.sdk/source/common/memorystream.cpp", "$sdk/public.sdk/source/vst/vstinitiids.cpp")
$sources += @('buffer.c','hook.c','trampoline.c','hde/hde64.c' | ForEach-Object { Join-Path $root "native/third_party/minhook/src/$_" })
$exe = Join-Path $OutputRoot 'p13_callback_split_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/I$QtDir/include" "/I$QtDir/include/QtCore" "/I$sdk" @sources "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib Ole32.lib User32.lib
if ($LASTEXITCODE) { throw 'P13 callback split test compilation failed.' }
$originalPath = $env:PATH
$originalPlatform = $env:QT_QPA_PLATFORM
try {
    $env:PATH = "$QtDir/bin;$env:PATH"
    $env:QT_QPA_PLATFORM = 'offscreen'
    & $exe
    if ($LASTEXITCODE) { throw 'P13 callback split test failed.' }
} finally { $env:PATH = $originalPath; $env:QT_QPA_PLATFORM = $originalPlatform }
