param([string]$AudioDll = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $AudioDll) { $AudioDll = Join-Path $env:ProgramFiles 'Arobas Music/Guitar Pro 8/AMAudio.dll' }
$AudioDll = (Resolve-Path -LiteralPath $AudioDll).Path
if ((Get-FileHash -LiteralPath $AudioDll -Algorithm SHA256).Hash -ne '0151B8D484A0DBEDBB74AA1A43975932349F812A2D8929DFAAD149EFBD992394') {
    throw 'Native SRC test requires the hash-gated GP 8.1.1.17 AMAudio.dll.'
}
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p13-native-src-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$exe = Join-Path $OutputRoot 'p13_native_src_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 "/I$(Join-Path $root 'native/modules')" (Join-Path $PSScriptRoot 'p13_native_src_test.cpp') "/Fo$OutputRoot/" "/Fe$exe"
if ($LASTEXITCODE) { throw 'P13 native SRC compilation failed.' }
& $exe $AudioDll
if ($LASTEXITCODE) { throw "P13 native SRC test failed: $LASTEXITCODE" }
