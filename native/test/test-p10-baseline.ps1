param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p10-baseline' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio x64 C++ build tools are required.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$sdk = Join-Path $root 'third_party/vst3sdk'
$includeArgs = @("/I$(Join-Path $root 'native/modules')", "/I$sdk")
$sources = @((Join-Path $root 'native/modules/audio_adapter.cpp'),
    (Join-Path $root 'native/modules/input_router.cpp'),
    (Join-Path $root 'native/modules/effect_chain.cpp'),
    (Join-Path $PSScriptRoot 'p10_diagnostics_test.cpp'))
$exe = Join-Path $OutputRoot 'p10_diagnostics_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @includeArgs @sources "/Fo$OutputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'P10 baseline fixture compilation failed.' }
$artifact = Join-Path $OutputRoot 'verification.json'
& $exe $artifact
if ($LASTEXITCODE) { throw 'P10 baseline fixture failed.' }
$evidence = Get-Content -LiteralPath $artifact -Raw | ConvertFrom-Json
if (-not $evidence.startup_timeline -or $evidence.startup_timeline.source -ne 'fixture' -or
    -not $evidence.audio_deadline -or -not $evidence.roundtrip_latency -or
    $evidence.input_route.output_written -ne $true -or $evidence.input_route.processed_blocks -lt 1) {
    throw 'P10 baseline evidence is incomplete.'
}
Write-Output "PASS: P10 baseline timeline/deadline/roundtrip/input evidence. Evidence: $OutputRoot"
