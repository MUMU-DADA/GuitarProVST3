param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [Parameter(Mandatory)][string]$ScorePath,
    [Parameter(Mandatory)][string]$SidecarPath,
    [Parameter(Mandatory)][string]$Vst3Root
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
. (Join-Path $PSScriptRoot 'host-session.ps1')
$ScorePath = (Resolve-Path -LiteralPath $ScorePath).Path
$SidecarPath = (Resolve-Path -LiteralPath $SidecarPath).Path
$run = Join-Path $root ('artifacts/p8-standalone-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run,(Join-Path $run 'mcp') | Out-Null
$fixture = Join-Path $run 'P8 Standalone.gp'
Copy-Item -LiteralPath $ScorePath -Destination $fixture
$chain = Get-Content -LiteralPath $SidecarPath -Raw | ConvertFrom-Json
$score = @($chain.scores.PSObject.Properties | Where-Object { [IO.Path]::GetFullPath($_.Name) -ieq $ScorePath })[0].Value
if (-not $score) { throw 'Source sidecar has no state for the supplied test score.' }
$chain.scores = @{($fixture.Replace('\','/'))=$score}
$chain | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'effect-chain.json') -Encoding UTF8
'{"enabled":false}' | Set-Content -LiteralPath (Join-Path $run 'mcp/settings.json') -Encoding UTF8
$before = Get-Gpvst3HostSnapshot $HostDirectory
$driverRoot = Join-Path $root '.tools/native/p8-standalone-driver/plugins'
if (-not (Test-Path (Join-Path $driverRoot 'generic/gpvst3_test_driver.dll'))) { throw 'Build the test-only driver with native/test/build-p8-standalone-driver.ps1.' }
$process = $null
$result = [ordered]@{run=$run;score=$fixture}
try {
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -Environment @{
        GPVST3_VST3_ROOT=$Vst3Root;GPVST3_RUNTIME_VST3=($Vst3Root -split ';')[0];GPVST3_ENABLE_P2_EFFECT='0'
        QT_PLUGIN_PATH=($driverRoot + ';' + (Split-Path -Parent (Split-Path -Parent ([IO.Path]::GetFullPath($PluginPath)))))
        QT_QPA_GENERIC_PLUGINS='gpvst3_test_driver';GPVST3_TEST_SCORE=$fixture
    }
    $statusPath = Join-Path $run 'status.json'; $observationPath = Join-Path $run 'p2-observation.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    do {
        Start-Sleep -Milliseconds 250
        if (Test-Path -LiteralPath $observationPath) { $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json }
    } while ((-not (Test-Path -LiteralPath $statusPath) -or -not $observation) -and [DateTime]::UtcNow -lt $deadline)
    $result.identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath
    $process.Refresh()
    $result.mcp_bridge_modules = @($process.Modules | Where-Object ModuleName -ieq 'guitarpro_mcp.dll' | ForEach-Object FileName)
    if ($result.mcp_bridge_modules.Count -or (Test-Path (Join-Path $run 'mcp/native-session.json'))) { throw 'Standalone process loaded an MCP bridge.' }
    if ($observation.gp_hook.track_binding_source -ne 'native_document_registry') { throw 'Independent native discovery was not selected.' }
    $expected = @($score.tracks.PSObject.Properties.Value | Where-Object { $_.present -and @($_.effects | Where-Object enabled).Count })
    $actual = @($observation.gp_hook.track_runtime_evidence | Where-Object configured_effects -gt 0)
    $result.before_play = $observation
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    do {
        Start-Sleep -Milliseconds 300
        $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
        $actual = @($observation.gp_hook.track_runtime_evidence | Where-Object configured_effects -gt 0)
        if ($actual.Count -eq $expected.Count -and -not @($actual | Where-Object { -not $_.processed -or -not $_.write_observed -or $_.error_blocks -gt 0 }).Count -and $observation.gp_hook.global_chain_process_blocks -gt 0) { break }
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($actual.Count -ne $expected.Count -or -not $observation.gp_hook.track_context_stable -or
        -not $observation.gp_hook.track_runtime_processed -or -not $observation.gp_hook.track_runtime_write_observed -or
        $observation.gp_hook.global_chain_process_blocks -lt 1 -or @($actual | Where-Object { -not $_.processed -or -not $_.write_observed -or $_.error_blocks -gt 0 }).Count) { throw 'Independent playback/writeback evidence was not observed.' }
    $result.after_play = $observation
    $process.Refresh()
    if (@($process.Modules | Where-Object ModuleName -ieq 'guitarpro_mcp.dll').Count) { throw 'MCP bridge appeared during playback.' }
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P8 independent discovery, all-track/global restore and real audio writeback with no MCP bridge. Evidence: $run"
}
catch {
    $result.failure = $_.Exception.Message
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    throw
}
finally {
    if ($process) {
        'stop' | Set-Content -LiteralPath (Join-Path $run 'driver-stop') -Encoding ASCII
        [Gpvst3TestProcess]::WaitForSingleObject($process.Handle, 5000) | Out-Null
    }
    try { Stop-Gpvst3TestHost $process -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
