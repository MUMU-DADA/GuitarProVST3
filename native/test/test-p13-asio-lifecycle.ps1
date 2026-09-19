param([string]$QtDir='', [string]$OutputRoot='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) {
    $QtDir=@((Join-Path $root '.tools/qt/5.15.3/msvc2019_64'),
        (Join-Path $root '.tools/qt/5.15.2/msvc2019_64'),
        (Join-Path $env:USERPROFILE 'source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64')) |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $QtDir) { throw 'Qt 5.15.x MSVC x64 SDK is required.' }
if (-not $OutputRoot) { $OutputRoot=Join-Path $root '.tools/native/p13-asio-lifecycle-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$minhook=Join-Path $root 'native/third_party/minhook'
$source=Join-Path $PSScriptRoot 'p13_asio_lifecycle_test.cpp'
$exe=Join-Path $OutputRoot 'p13_asio_lifecycle_test.exe'
$lib=Join-Path $OutputRoot 'minhook.lib'
$originalPath=$env:PATH
try {
    & cl /nologo /TC /MD /O2 "/I$minhook/include" /c "$minhook/src/buffer.c" "$minhook/src/hook.c" "$minhook/src/trampoline.c" "$minhook/src/hde/hde64.c" "/Fo$OutputRoot/"
    if ($LASTEXITCODE) { throw 'MinHook compilation failed.' }
    & lib /nologo "/out:$lib" "$OutputRoot/buffer.obj" "$OutputRoot/hook.obj" "$OutputRoot/trampoline.obj" "$OutputRoot/hde64.obj"
    if ($LASTEXITCODE) { throw 'MinHook archive failed.' }
    & cl /nologo /std:c++17 /EHsc /MD /O2 /W4 /WX /utf-8 /DQT_NO_DEBUG /DUNICODE /D_UNICODE "/I$QtDir/include" "/I$minhook/include" $source "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib $lib
    if ($LASTEXITCODE) { throw 'ASIO lifecycle proxy test compilation failed.' }
    $env:PATH=(Join-Path $QtDir 'bin')+';'+$originalPath
    & $exe
    if ($LASTEXITCODE) { throw 'ASIO lifecycle proxy test failed.' }
} finally { $env:PATH=$originalPath }
