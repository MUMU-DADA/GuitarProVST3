param(
    [string]$QtDir = '',
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $QtDir) {
    $candidates = @(
        (Join-Path $projectRoot '.tools/qt/5.15.3/msvc2019_64'),
        (Join-Path $projectRoot '.tools/qt/5.15.2/msvc2019_64'),
        (Join-Path $env:USERPROFILE 'source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64')
    )
    $QtDir = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $QtDir -or -not (Test-Path -LiteralPath $QtDir)) { throw 'Qt 5.15.x MSVC x64 SDK not found. Pass -QtDir explicitly.' }
$QtDir = (Resolve-Path -LiteralPath $QtDir).Path

if (-not $OutputRoot) { $OutputRoot = Join-Path $projectRoot '.tools/native' }
$buildDir = Join-Path $OutputRoot 'build'
$pluginDir = Join-Path $OutputRoot 'plugins/imageformats'
New-Item -ItemType Directory -Force -Path $buildDir,$pluginDir | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

$includeDirs = @(
    (Join-Path $QtDir 'include'),
    (Join-Path $QtDir 'include/QtCore'),
    (Join-Path $QtDir 'include/QtGui'),
    (Join-Path $PSScriptRoot 'modules'),
    $buildDir
)
$includeArgs = $includeDirs | ForEach-Object { "-I$_" }
$clIncludeArgs = $includeDirs | ForEach-Object { "/I$_" }
$moc = Join-Path $QtDir 'bin/moc.exe'
& $moc @includeArgs (Join-Path $PSScriptRoot 'vst3_autoload.cpp') -o (Join-Path $buildDir 'vst3_autoload.moc')
if ($LASTEXITCODE) { throw 'Qt moc failed.' }

$sources = @(
    (Join-Path $PSScriptRoot 'vst3_autoload.cpp'),
    (Join-Path $PSScriptRoot 'modules/bootstrap.cpp'),
    (Join-Path $PSScriptRoot 'modules/state_manager.cpp')
)
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /LD /DQT_NO_DEBUG /DQT_PLUGIN @clIncludeArgs @sources "/Fo$buildDir/" "/Fd$buildDir/guitarpro_vst3_autoload.pdb" "/Fe$pluginDir/guitarpro_vst3_autoload.dll" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib "/IMPLIB:$buildDir/guitarpro_vst3_autoload.lib"
if ($LASTEXITCODE) { throw 'P0 plugin compilation failed.' }
Write-Output "Built $pluginDir/guitarpro_vst3_autoload.dll"
