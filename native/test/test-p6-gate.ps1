param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$QtDir = '',
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path -LiteralPath (Join-Path $HostDirectory 'GuitarPro.exe'))) {
    throw "Host not found: $HostDirectory"
}
if (-not $QtDir) {
    $QtDir = @(
        (Join-Path $root '.tools/qt/5.15.3/msvc2019_64'),
        (Join-Path $root '.tools/qt/5.15.2/msvc2019_64'),
        (Join-Path $env:USERPROFILE 'source/GuitarProMCP/.tools/qt/5.15.2/msvc2019_64')
    ) | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'include/QtCore/QCoreApplication') } | Select-Object -First 1
}
if (-not $QtDir -or -not (Test-Path -LiteralPath (Join-Path $QtDir 'include/QtCore/QCoreApplication'))) {
    throw 'Qt 5.15.x MSVC x64 SDK not found. Pass -QtDir explicitly.'
}
$QtDir = (Resolve-Path -LiteralPath $QtDir).Path
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p6-gate-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio x64 C++ build tools are required.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio x64 C++ build tools are required.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

$include = @("/I$(Join-Path $QtDir 'include')", "/I$(Join-Path $QtDir 'include/QtCore')",
             "/I$(Join-Path $root 'native/modules')")
$source = Join-Path $PSScriptRoot 'p6_host_lock_test.cpp'
$exe = Join-Path $OutputRoot 'p6_host_lock_test.exe'
& cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 @include $source "/Fo$OutputRoot/" "/Fe$exe" /link "/LIBPATH:$(Join-Path $QtDir 'lib')" Qt5Core.lib
if ($LASTEXITCODE) { throw 'P6 host lock test compilation failed.' }
$env:PATH = "$(Join-Path $QtDir 'bin');$env:PATH"
& $exe $HostDirectory (Join-Path $root 'native/host_manifest.json')
if ($LASTEXITCODE) { throw 'P6 host hash gate test failed.' }

$manifest = Get-Content -LiteralPath (Join-Path $root 'native/host_manifest.json') -Raw | ConvertFrom-Json
$expected = @{}
foreach ($property in $manifest.files.PSObject.Properties) { $expected[$property.Name] = $property.Value }
if ($expected.Count -ne 5) { throw 'P6 host manifest must contain five locked files.' }
$actual = @{}
foreach ($name in $expected.Keys) { $actual[$name] = (Get-FileHash -LiteralPath (Join-Path $HostDirectory $name) -Algorithm SHA256).Hash }
@{passed=$true;host_directory=(Resolve-Path $HostDirectory).Path;manifest=$expected;actual=$actual;negative_case='GuitarPro.exe appended bytes';test_sha256=(Get-FileHash $source).Hash} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P6 host hash gate positive/negative verification. Evidence: $OutputRoot"
