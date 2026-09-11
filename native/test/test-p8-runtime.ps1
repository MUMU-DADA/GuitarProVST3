param([string]$QtDir = '', [string]$OutputRoot = '', [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8', [string]$PluginPath = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p8-runtime-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'build-p7-gain-fixture.ps1') -QtDir $QtDir -OrderFixture
if ($LASTEXITCODE) { throw 'P8 order fixture build failed.' }
$sdk = Join-Path $root 'third_party/vst3sdk'
$sources = @((Join-Path $PSScriptRoot 'p8_runtime_test.cpp')) + @('audio_adapter','effect_chain','input_router','state_manager' | ForEach-Object { Join-Path $root "native/modules/$_.cpp" }) +
    @("$sdk/pluginterfaces/base/coreiids.cpp", "$sdk/pluginterfaces/base/funknown.cpp", "$sdk/pluginterfaces/base/ustring.cpp", "$sdk/public.sdk/source/common/memorystream.cpp", "$sdk/public.sdk/source/vst/vstinitiids.cpp")
$dll = Join-Path $OutputRoot 'p8_runtime_test.dll'
& cl /nologo /LD /std:c++17 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/I$QtDir/include" "/I$QtDir/include/QtCore" "/I$sdk" @sources "/Fo$OutputRoot/" "/Fe$dll" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib Ole32.lib User32.lib "/IMPLIB:$OutputRoot/runtime-test.lib"
if ($LASTEXITCODE) { throw 'P8 runtime fixture build failed.' }
& (Join-Path $PSScriptRoot 'build-p8-standalone-driver.ps1') -QtDir $QtDir
. (Join-Path $PSScriptRoot 'host-session.ps1')
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
$run = Join-Path $root ('artifacts/p8-runtime-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run,(Join-Path $run 'mcp') | Out-Null
'{"enabled":false}' | Set-Content -LiteralPath (Join-Path $run 'mcp/settings.json') -Encoding UTF8
$before = Get-Gpvst3HostSnapshot $HostDirectory
$process = $null
try {
    $driverRoot = Join-Path $root '.tools/native/p8-standalone-driver/plugins'
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -Environment @{
        QT_PLUGIN_PATH=($driverRoot + ';' + (Split-Path -Parent (Split-Path -Parent ([IO.Path]::GetFullPath($PluginPath)))))
        QT_QPA_GENERIC_PLUGINS='gpvst3_test_driver';GPVST3_TEST_RUNTIME_DLL=[IO.Path]::GetFullPath($dll)
        GPVST3_TEST_RUNTIME_FIXTURE=(Join-Path $root '.tools/native/p8-order-test/P8 Order Fixture.vst3')
        GPVST3_VST3_ROOT=(Join-Path $run 'empty-catalog')
    }
    $resultPath = Join-Path $run 'runtime-test.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do { Start-Sleep -Milliseconds 200 } while (-not (Test-Path $resultPath) -and [DateTime]::UtcNow -lt $deadline)
    if (-not (Test-Path $resultPath) -or (Get-Content $resultPath -Raw | ConvertFrom-Json).result -ne 0) { throw "P8 in-Guitar Pro runtime behavior verification failed. Evidence: $run" }
    Write-Output "PASS: P8 in-Guitar Pro real VST3 rate/state/failure behavior fixture. Evidence: $run"
} finally {
    if ($process) { [Gpvst3TestProcess]::WaitForSingleObject($process.Handle, 5000) | Out-Null }
    try { Stop-Gpvst3TestHost $process -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
