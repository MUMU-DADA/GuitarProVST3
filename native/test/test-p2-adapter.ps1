$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputRoot = Join-Path $root '.tools/native/p2-adapter-test'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio x64 C++ build tools are required.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

$sdk = Join-Path $root 'third_party/vst3sdk'
$include = @("/I$(Join-Path $root 'native/modules')", "/I$sdk")
$sources = @(
    (Join-Path $root 'native/modules/audio_adapter.cpp'),
    (Join-Path $PSScriptRoot 'p2_audio_adapter_test.cpp')
)
$exe = Join-Path $outputRoot 'p2_audio_adapter_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @include @sources "/Fo$outputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'P2 audio adapter test compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'P2 audio adapter test failed.' }
Write-Output "PASS: P2 adapter isolated verification. Evidence: $outputRoot"
