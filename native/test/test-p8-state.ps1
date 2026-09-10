param([string]$QtDir = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p8-state-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$include = @("/I$(Join-Path $QtDir 'include')", "/I$(Join-Path $QtDir 'include/QtCore')", "/I$(Join-Path $root 'native/modules')")
$sources = @((Join-Path $root 'native/modules/state_manager.cpp'), (Join-Path $PSScriptRoot 'p8_state_test.cpp'))
$exe = Join-Path $OutputRoot 'p8_state_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @include @sources "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$(Join-Path $QtDir 'lib')" Qt5Core.lib
if ($LASTEXITCODE) { throw 'P8 state fixture compilation failed.' }
$env:PATH = "$(Join-Path $QtDir 'bin');$env:PATH"
& $exe
if ($LASTEXITCODE) { throw 'P8 state regression failed.' }
