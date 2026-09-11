param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p9-switch-test' }
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
    (Join-Path $PSScriptRoot 'p9_switch_test.cpp'))
$exe = Join-Path $OutputRoot 'p9_switch_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 "/I$(Join-Path $root 'native/modules')" "/I$(Join-Path $root 'third_party/vst3sdk')" @sources "/Fo$OutputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'P9 switch fixture compilation failed.' }
$artifact = Join-Path $OutputRoot 'p9-switch.json'
& $exe $artifact
if ($LASTEXITCODE) { throw 'P9 switch regression failed.' }
if (-not (Test-Path -LiteralPath $artifact)) { throw 'P9 switch artifact was not written.' }
$record = Get-Content -LiteralPath $artifact -Raw | ConvertFrom-Json
if ($record.sequence_gaps -ne 0 -or $record.switch_count -lt 201 -or $record.ramp_samples -ne 64) {
    throw 'P9 switch artifact did not meet continuity and handoff thresholds.'
}
Write-Output "PASS: P9 switch fixture, sequence continuity and fallback metrics. Evidence: $OutputRoot"
