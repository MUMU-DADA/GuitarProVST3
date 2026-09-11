param([string]$QtDir = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p8-standalone-driver' }
$plugin = Join-Path $OutputRoot 'plugins/generic'
New-Item -ItemType Directory -Force -Path $plugin | Out-Null
'{"Keys":["gpvst3_test_driver"]}' | Set-Content -LiteralPath (Join-Path $OutputRoot 'p8-test-driver.json') -Encoding UTF8
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$source = Join-Path $PSScriptRoot 'p8_standalone_driver.cpp'
& "$QtDir/bin/moc.exe" "-I$QtDir/include" "-I$OutputRoot" $source -o "$OutputRoot/p8_standalone_driver.moc"
if ($LASTEXITCODE) { throw 'Standalone driver moc failed.' }
& cl /nologo /LD /std:c++17 /EHsc /MD /O2 /utf-8 /DQT_NO_DEBUG /DQT_PLUGIN "/I$QtDir/include" "/I$OutputRoot" $source "/Fo$OutputRoot/" "/Fe$plugin/gpvst3_test_driver.dll" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib "/IMPLIB:$OutputRoot/driver.lib"
if ($LASTEXITCODE) { throw 'Standalone driver build failed.' }
