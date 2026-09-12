param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p10-activation-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$sources = @(
    (Join-Path $root 'native/modules/audio_adapter.cpp'),
    (Join-Path $root 'native/modules/effect_chain.cpp'),
    (Join-Path $PSScriptRoot 'p10_activation_test.cpp'))
$exe = Join-Path $OutputRoot 'p10_activation_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 "/I$(Join-Path $root 'native/modules')" "/I$(Join-Path $root 'third_party/vst3sdk')" @sources "/Fo$OutputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'P10 activation fixture compilation failed.' }
$artifact = Join-Path $OutputRoot 'p10-activation.json'
& $exe $artifact
if ($LASTEXITCODE) { throw 'P10 activation regression failed.' }
$record = Get-Content -LiteralPath $artifact -Raw | ConvertFrom-Json
if (-not $record.bypass_next_callback -or $record.sequence_gaps -ne 0 -or
    $record.cold_callbacks_to_first -gt 2 -or $record.warm_callbacks_to_first -gt 2) {
    throw 'P10 activation artifact did not meet bypass/first-callback thresholds.'
}
Write-Output "PASS: P10 activation fixture, immediate bypass and warm/cold first callback evidence. Evidence: $OutputRoot"
