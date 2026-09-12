param([string]$OutputRoot = '', [string]$PluginPath = '', [string]$Vst3Root = '', [switch]$RunHost)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p10-startup' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p10-baseline.ps1') -OutputRoot (Join-Path $OutputRoot 'fixture')
$timeline = Get-Content (Join-Path $OutputRoot 'fixture/verification.json') -Raw | ConvertFrom-Json
if ($RunHost) {
    $hostDir = 'C:\Program Files\Arobas Music\Guitar Pro 8'
    $mcpRoot = Join-Path (Split-Path -Parent $root) 'GuitarProMCP'
    if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p10-release-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
    $plugin = [IO.Path]::GetFullPath($PluginPath)
    if (-not (Test-Path -LiteralPath $plugin)) { throw 'Build the P10 plugin before -RunHost.' }
    if (-not $Vst3Root) {
        $fixtureRoot = Join-Path $root '.tools/native/p10-gain-fixture'
        $fixture = Join-Path $fixtureRoot 'P7 Gain Fixture.vst3'
        if (-not (Test-Path -LiteralPath $fixture)) {
            & (Join-Path $PSScriptRoot 'build-p7-gain-fixture.ps1') -OutputRoot $fixtureRoot
            if ($LASTEXITCODE) { throw 'P10 startup Gain Fixture build failed.' }
        }
        $Vst3Root = $fixture
    }
    $hostRun = Join-Path $OutputRoot 'host'
    & (Join-Path $PSScriptRoot 'test-p8-track-runtime.ps1') -HostDirectory $hostDir -McpRoot $mcpRoot -PluginPath $plugin -Vst3Root $Vst3Root -OutputRoot $hostRun -CheckLifecycle -ExpectedBindingSource mcp_context_native_registry
    if ($LASTEXITCODE) { throw 'MCP startup/runtime regression failed.' }
    $hostStatus = Get-Content (Join-Path $hostRun 'status.json') -Raw | ConvertFrom-Json
    $hostTimeline = $hostStatus.startup_timeline
    if (-not $hostTimeline -or $hostTimeline.elapsed_ms -le 0 -or
        $hostTimeline.hook_ready_ms -lt 0 -or $hostTimeline.scan_scheduled_ms -lt $hostTimeline.hook_ready_ms -or
        $hostTimeline.ui_ready_ms -lt $hostTimeline.scan_scheduled_ms) {
        throw "MCP host startup timeline is incomplete: $($hostTimeline | ConvertTo-Json -Compress)"
    }
}
@{mode=$(if ($RunHost) {'fixture_plus_mcp_host'} else {'fixture_only'});
  timeline=$timeline.startup_timeline;host_timeline=$(if ($RunHost) {$hostTimeline} else {$null});
  host_validation=[bool]$RunHost} |
    ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P10 startup timeline and deferred initialization evidence. Evidence: $OutputRoot"
