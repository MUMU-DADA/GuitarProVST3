param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = '',
    [string]$PluginPath = '',
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not (Test-Path -LiteralPath $PluginPath)) { throw 'Build the plugin first with native/build.ps1.' }

# test-p2-runtime enables the hash-gated hook and the worker-created runtime
# chain, then checks live playback, atomic slot switching, timing counters,
# error fallback state, and all 44.1/48/96 kHz × 64/128/256 reconfigurations.
$runtime = Join-Path $PSScriptRoot 'test-p2-runtime.ps1'
& $runtime -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost
if ($LASTEXITCODE) { throw 'P3 runtime verification failed.' }
& $runtime -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost -ExpectP3Fallback
if ($LASTEXITCODE) { throw 'P3 error fallback verification failed.' }
& $runtime -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost -ExpectP3TotalBypass
if ($LASTEXITCODE) { throw 'P3 total bypass verification failed.' }
Write-Output 'PASS: P3 realtime chain safety, bypass/error counters and reconfiguration matrix.'
