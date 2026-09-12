param([string]$OutputRoot = '', [string]$PluginPath = '', [string]$Vst3Root = '', [switch]$RunHost)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p11-startup' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
if ($RunHost) {
    $hostDir = 'C:\Program Files\Arobas Music\Guitar Pro 8'
    $mcpRoot = Join-Path (Split-Path -Parent $root) 'GuitarProMCP'
    if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
    if (-not $Vst3Root) { $Vst3Root = Join-Path $root '.tools/native/p10-gain-fixture/P7 Gain Fixture.vst3' }
    & (Join-Path $PSScriptRoot 'test-p10-startup.ps1') -OutputRoot (Join-Path $OutputRoot 'startup') -PluginPath $PluginPath -Vst3Root $Vst3Root -RunHost:$false
    # P11 validates startup and the real callback route here. Track duplication
    # semantics belong to the P8 lifecycle suite and must not mask P11 evidence.
    & (Join-Path $PSScriptRoot 'test-p8-track-runtime.ps1') -HostDirectory $hostDir -McpRoot $mcpRoot -PluginPath $PluginPath -Vst3Root $Vst3Root -OutputRoot (Join-Path $OutputRoot 'host') -ExpectedBindingSource mcp_context_native_registry -HookMode enabled -P4Route input_insert
} else {
    & (Join-Path $PSScriptRoot 'test-p10-startup.ps1') -OutputRoot (Join-Path $OutputRoot 'startup') -PluginPath $PluginPath -Vst3Root $Vst3Root
}
$e = Get-Content (Join-Path $OutputRoot 'startup/verification.json') -Raw | ConvertFrom-Json
if (-not $e.timeline) { throw 'P11 startup timeline is missing.' }
$hostEvidence = if ($RunHost) { Get-Content (Join-Path $OutputRoot 'host/verification.json') -Raw | ConvertFrom-Json } else { $null }
if ($RunHost -and (-not $hostEvidence.identity.plugin_path -or [IO.Path]::GetFullPath($hostEvidence.identity.plugin_path) -ine [IO.Path]::GetFullPath($PluginPath))) { throw 'MCP host did not load the requested P11 DLL.' }
if ($RunHost) {
    $status = Get-Content (Join-Path $OutputRoot 'host/status.json') -Raw | ConvertFrom-Json
    $hook = $status.gp_hook
    if (-not $hook.audio_output_callback.call_observed -or $hook.audio_output_callback.call_count -lt 1 -or
        $hook.input_route -ne 'input_insert' -or $hook.input_processed_blocks -lt 1 -or
        -not $hook.input_processor_ready -or $hook.track_chain_processed_blocks -lt 1 -or
        -not $hook.input_interleaved_output_written) {
        throw "P11 real-host callback evidence is incomplete: $($hook | ConvertTo-Json -Depth 8 -Compress)"
    }
}
@{complete=$true; host_validation=[bool]$RunHost; deferred_track_context=$true; evidence=$e; host_evidence=$hostEvidence} |
  ConvertTo-Json -Depth 30 | Set-Content (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P11 startup/score-open deferred lifecycle evidence. Evidence: $OutputRoot"
