param([string]$QtDir = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p13-input-runtime-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$fixtureRoot = Join-Path $OutputRoot 'fixture'
& (Join-Path $PSScriptRoot 'build-p7-gain-fixture.ps1') -QtDir $QtDir -OutputRoot $fixtureRoot -OrderFixture
if ($LASTEXITCODE) { throw 'P13 input runtime VST3 fixture build failed.' }
$sdk = Join-Path $root 'third_party/vst3sdk'
$sources = @((Join-Path $PSScriptRoot 'p13_input_runtime_test.cpp')) + @('audio_adapter','effect_chain','input_router','state_manager' | ForEach-Object { Join-Path $root "native/modules/$_.cpp" }) +
    @("$sdk/pluginterfaces/base/coreiids.cpp", "$sdk/pluginterfaces/base/funknown.cpp", "$sdk/pluginterfaces/base/ustring.cpp", "$sdk/public.sdk/source/common/memorystream.cpp", "$sdk/public.sdk/source/vst/vstinitiids.cpp")
$sources += @('buffer.c','hook.c','trampoline.c','hde/hde64.c' | ForEach-Object { Join-Path $root "native/third_party/minhook/src/$_" })
$exe = Join-Path $OutputRoot 'p13_input_runtime_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/I$QtDir/include" "/I$QtDir/include/QtCore" "/I$sdk" @sources "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib Ole32.lib User32.lib
if ($LASTEXITCODE) { throw 'P13 input runtime fixture build failed.' }
$previousPath = $env:PATH
$previousPlatform = $env:QT_QPA_PLATFORM
try {
    $env:PATH = "$QtDir/bin;$env:PATH"
    $env:QT_QPA_PLATFORM = 'offscreen'
    & $exe (Join-Path $fixtureRoot 'P8 Order Fixture.vst3')
    if ($LASTEXITCODE) { throw 'P13 independent input runtime regression failed.' }
} finally {
    $env:PATH = $previousPath
    $env:QT_QPA_PLATFORM = $previousPlatform
}
