param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p12-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p12-lazy-startup' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
. (Join-Path $PSScriptRoot 'host-session.ps1')
$before = Get-Gpvst3HostSnapshot $HostDirectory
$process = $null
try {
    $environment = @{ GPVST3_ENABLE_P2_HOOK = '0'; GPVST3_ENABLE_P2_EFFECT = '0' }
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $OutputRoot -Environment $environment
    $statusPath = Join-Path $OutputRoot 'status.json'
    $observationPath = Join-Path $OutputRoot 'p2-observation.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not (Test-Path -LiteralPath $statusPath) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if ($process.HasExited) { throw 'Guitar Pro exited before P12 startup status was published.' }
    }
    if (-not (Test-Path -LiteralPath $statusPath)) { throw 'P12 startup status was not published.' }
    Start-Sleep -Milliseconds 1200
    if (-not (Test-Path -LiteralPath $observationPath)) { throw 'P12 runtime observation was not published.' }
    $status = Get-Content -LiteralPath $statusPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $observation = Get-Content -LiteralPath $observationPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $hook = $observation.gp_hook
    $vst3 = $status.vst3_host
    if (@($hook.instances).Count -ne 0 -or $hook.score_open -ne $false -or
        $hook.preload_pending -ne $false -or $vst3.instances_created -ne 0) {
        throw "No-score startup instantiated runtime objects: $($status | ConvertTo-Json -Depth 8 -Compress)"
    }
    @{schema=1;status='pass';score_open=[bool]$hook.score_open;instances=@($hook.instances).Count;
      vst3_instances_created=$vst3.instances_created;preload_pending=[bool]$hook.preload_pending} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
    Write-Output "PASS: P12 no-score startup remains metadata-only. Evidence: $OutputRoot"
} finally {
    if ($process) {
        try { Stop-Gpvst3TestHost $process -RunDirectory $OutputRoot } catch { }
        try { Assert-Gpvst3HostUnchanged $before $HostDirectory $OutputRoot } catch { throw }
    }
}
