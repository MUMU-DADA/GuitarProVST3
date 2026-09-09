param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [string]$McpRoot = '',
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'
$p0 = Join-Path $PSScriptRoot 'test-p0.ps1'
& $p0 -HostDirectory $HostDirectory -PluginPath $PluginPath -KeepHost:$KeepHost -RequireP1 -RequireP2 -RequireP2Hook
if ($LASTEXITCODE) { throw 'P2 verification failed.' }
$runtime = Join-Path $PSScriptRoot 'test-p2-runtime.ps1'
& $runtime -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost
if ($LASTEXITCODE) { throw 'P2 runtime verification failed.' }
Write-Output 'PASS: P2 adapter, gated hook and real-host VST3 processing.'
