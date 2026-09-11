param([string]$QtDir = '', [string]$OutputRoot = '', [switch]$OrderFixture)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root $(if ($OrderFixture) {'.tools/native/p8-order-test'} else {'.tools/native/p7-gain-test'}) }
$fixtureName = if ($OrderFixture) {'P8 Order Fixture'} else {'P7 Gain Fixture'}
$bundle = Join-Path $OutputRoot ($fixtureName + '.vst3')
$bin = Join-Path $bundle 'Contents/x86_64-win'
New-Item -ItemType Directory -Force -Path $bin,(Join-Path $bundle 'Contents/Resources') | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$sdk = Join-Path $root 'third_party/vst3sdk'
$sources = @((Join-Path $PSScriptRoot 'p7_gain_fixture.cpp'), "$sdk/pluginterfaces/base/coreiids.cpp", "$sdk/pluginterfaces/base/funknown.cpp", "$sdk/pluginterfaces/base/ustring.cpp", "$sdk/public.sdk/source/vst/vstinitiids.cpp")
$defines = @()
if ($OrderFixture) { $defines += '/DP8_ORDER_FIXTURE' }
& cl /nologo /LD /std:c++17 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS @defines "/I$QtDir/include" "/I$sdk" @sources "/Fo$OutputRoot/" "/Fe$bin/$fixtureName.vst3" /link "/LIBPATH:$QtDir/lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib Ole32.lib "/IMPLIB:$OutputRoot/gain.lib"
if ($LASTEXITCODE) { throw 'Gain fixture build failed.' }
$classes = if ($OrderFixture) {
    for ($index = 0; $index -lt 3; $index++) {
        @{CID=('{0:X8}506070801122334455667788' -f (0x10203041 + $index));Category='Audio Module Class';Name=@('P8 A Offset','P8 B Gain','P8 C Offset')[$index];Vendor='Test';'Sub Categories'=@('Fx')}
    }
} else { @(@{CID='10203040506070801122334455667788';Category='Audio Module Class';Name='P7 Gain Fixture';Vendor='Test';'Sub Categories'=@('Fx')}) }
@{Name=$fixtureName;Classes=@($classes)} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $bundle 'Contents/Resources/moduleinfo.json') -Encoding UTF8
Write-Output "Built test-only VST3: $bundle"
