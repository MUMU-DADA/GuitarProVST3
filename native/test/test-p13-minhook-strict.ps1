param([string]$OutputRoot='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot=Join-Path $root '.tools/native/p13-minhook-strict-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$minhook=Join-Path $root 'native/third_party/minhook'
$exe=Join-Path $OutputRoot 'p13_minhook_strict_test.exe'
& cl /nologo /TC /MD /O2 /W3 /WX /utf-8 "$PSScriptRoot/p13_minhook_strict_test.c" "$minhook/src/buffer.c" "$minhook/src/trampoline.c" "$minhook/src/hde/hde64.c" "/Fo$OutputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'Strict MinHook test compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'Strict MinHook test failed.' }
