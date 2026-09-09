$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $root '.tools/native/p4-test'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio x64 C++ build tools are required.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

$sdk = Join-Path $root 'third_party/vst3sdk'
$includeArgs = @(
    "/I$(Join-Path $root 'native/modules')"
    "/I$sdk"
)
$sources = @(
    (Join-Path $root 'native/modules/audio_adapter.cpp'),
    (Join-Path $root 'native/modules/input_router.cpp'),
    (Join-Path $root 'native/tests/p4_input_router_test.cpp')
)
$exe = Join-Path $outputRoot 'p4_input_router_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @includeArgs @sources "/Fo$outputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'P4 router test compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'P4 router test failed.' }
Write-Output "PASS: P4 router isolated verification. Evidence: $outputRoot"
