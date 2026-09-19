param([string]$OutputRoot = '')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p13-drain-probe-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio x64 C++ build tools are required.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

$source = Join-Path $PSScriptRoot 'p13_input_drain_probe_test.cpp'
$exe = Join-Path $OutputRoot 'p13_input_drain_probe_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 "/I$(Join-Path $root 'native/modules')" $source "/Fo$OutputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'P13 drain probe compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'P13 drain probe test failed.' }
Write-Output "PASS: P13 experimental snapshot reader; synthetic memory tests only. Evidence: $OutputRoot"
