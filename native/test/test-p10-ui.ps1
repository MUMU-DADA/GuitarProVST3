param([string]$QtDir = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p10-ui' }
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not (Test-Path -LiteralPath $QtDir)) { throw "Qt SDK not found: $QtDir" }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$include = @("/I$(Join-Path $root 'native/modules')", "/I$(Join-Path $root 'third_party/vst3sdk')",
    "/I$(Join-Path $QtDir 'include')", "/I$(Join-Path $QtDir 'include/QtCore')", "/I$(Join-Path $QtDir 'include/QtGui')", "/I$(Join-Path $QtDir 'include/QtWidgets')")
$sources = @((Join-Path $root 'native/modules/state_manager.cpp'), (Join-Path $root 'native/modules/qt_ui.cpp'),
    (Join-Path $PSScriptRoot 'p10_ui_test.cpp'))
$exe = Join-Path $OutputRoot 'p10_ui_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /DQT_NO_DEBUG /DUNICODE /D_UNICODE @include @sources "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib
if ($LASTEXITCODE) { throw 'P10 UI fixture compilation failed.' }
$env:PATH = "$QtDir/bin;$env:PATH"
& $exe | Tee-Object -Variable output | Out-Host
if ($LASTEXITCODE) { throw 'P10 UI fixture failed.' }
@{result=($output -join ' ');track_switch=$true;identity='Track N · VST3 (N)'} |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P10 title track identity and switch regression. Evidence: $OutputRoot"
