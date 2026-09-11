param([string]$QtDir = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p8-ui-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$include = @("/I$(Join-Path $QtDir 'include')", "/I$(Join-Path $QtDir 'include/QtCore')", "/I$(Join-Path $QtDir 'include/QtGui')", "/I$(Join-Path $QtDir 'include/QtWidgets')", "/I$(Join-Path $root 'native/modules')")
$sources = @((Join-Path $root 'native/modules/state_manager.cpp'), (Join-Path $root 'native/modules/qt_ui.cpp'), (Join-Path $PSScriptRoot 'p8_ui_test.cpp'))
$exe = Join-Path $OutputRoot 'p8_ui_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @include @sources "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$(Join-Path $QtDir 'lib')" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib
if ($LASTEXITCODE) { throw 'P8 UI fixture compilation failed.' }
$env:PATH = "$(Join-Path $QtDir 'bin');$env:PATH"
$previousScale = $env:QT_SCALE_FACTOR
try {
    foreach ($scale in @('1', '1.25', '1.5')) {
        $env:QT_SCALE_FACTOR = $scale
        & $exe
        if ($LASTEXITCODE) { throw "P8 UI regression failed at scale $scale." }
    }
} finally { $env:QT_SCALE_FACTOR = $previousScale }
