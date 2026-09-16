param(
    [string]$QtDir = '',
    [string]$OutputRoot = '',
    [string]$PluginPath = '',
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$Vst3Root = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p12-suite' }
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p12-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not $QtDir) { $QtDir = Join-Path (Split-Path -Parent $root) 'GuitarProMCP/.tools/qt/5.15.2/msvc2019_64' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

& (Join-Path $PSScriptRoot 'test-p12-lazy-startup.ps1') -HostDirectory $HostDirectory -PluginPath $PluginPath -OutputRoot (Join-Path $OutputRoot 'lazy-startup')
& (Join-Path $PSScriptRoot 'test-p11.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'p11')

$fixture = $Vst3Root
if (-not $fixture) { $fixture = Join-Path $root '.tools/native/p12-gain-fixture/P7 Gain Fixture.vst3' }
$resolvedPlugin = (Resolve-Path -LiteralPath $PluginPath).Path
$resolvedFixture = (Resolve-Path -LiteralPath $fixture).Path
$common = @{PluginPath=$resolvedPlugin;Vst3Root=$resolvedFixture;ExpectedBindingSource='mcp_context_native_registry';HookMode='enabled';HostDirectory=$HostDirectory;McpRoot=$McpRoot}
& (Join-Path $PSScriptRoot 'test-p8-track-runtime.ps1') @common -CheckP12
& (Join-Path $PSScriptRoot 'test-p8-track-runtime.ps1') @common -CheckLifecycle

@{schema=1;lazy_startup='pass';p11='pass';selection='pass';lifecycle='pass'} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P12 event-driven selection, lazy runtime and lifecycle suite. Evidence: $OutputRoot"
