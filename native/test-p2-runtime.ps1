param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = '',
    [string]$PluginPath = '',
    [switch]$KeepHost,
    [switch]$ExpectP3Fallback,
    [switch]$ExpectP3TotalBypass,
    [switch]$EnableP4,
    [ValidateSet('input_insert','bus_mix')]
    [string]$P4Route = 'bus_mix'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $McpRoot) { $McpRoot = Join-Path (Split-Path -Parent $root) 'GuitarProMCP' }
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
$mcpGeneric = Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll'
$mcpAutoload = Join-Path $McpRoot '.tools/native/plugins/imageformats/guitarpro_mcp_autoload.dll'
$mcpClient = Join-Path $McpRoot 'native/mcp-client.ps1'
foreach ($path in @($PluginPath, $mcpGeneric, $mcpAutoload, $mcpClient, (Join-Path $HostDirectory 'GuitarPro.exe'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required runtime test file not found: $path" }
}

$run = Join-Path $root ('artifacts/p2-runtime-' + [guid]::NewGuid().ToString('N'))
$hostCopy = Join-Path $root ('.tools/p2-runtime-host-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run,$hostCopy | Out-Null
Get-ChildItem -LiteralPath $HostDirectory -File | Where-Object { $_.Extension -in '.dll','.conf' -or $_.Name -eq 'GuitarPro.exe' } | Copy-Item -Destination $hostCopy
Copy-Item -LiteralPath (Join-Path $HostDirectory 'Plugins') -Destination $hostCopy -Recurse
if (Test-Path -LiteralPath (Join-Path $HostDirectory 'translations')) { Copy-Item -LiteralPath (Join-Path $HostDirectory 'translations') -Destination $hostCopy -Recurse }
$imageDir = Join-Path $hostCopy 'Plugins/imageformats'
$genericDir = Join-Path $hostCopy 'Plugins/generic'
New-Item -ItemType Directory -Force -Path $imageDir,$genericDir | Out-Null
Remove-Item -LiteralPath (Join-Path $imageDir 'guitarpro_mcp_autoload.dll') -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $PluginPath -Destination (Join-Path $imageDir 'guitarpro_vst3_autoload.dll')
Copy-Item -LiteralPath $mcpAutoload -Destination (Join-Path $imageDir 'guitarpro_mcp_autoload.dll')
Copy-Item -LiteralPath $mcpGeneric -Destination (Join-Path $genericDir 'guitarpro_mcp.dll')

$fixture = Join-Path $run 'runtime.gp'
Copy-Item -LiteralPath (Join-Path $McpRoot 'native/testdata/minimal.gp') -Destination $fixture
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($fixture, [IO.Compression.ZipArchiveMode]::Update)
try {
    $entry = $archive.GetEntry('Content/score.gpif')
    $reader = [IO.StreamReader]::new($entry.Open())
    try { [xml]$gpif = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $gpif.GPIF.Tracks.Track.AudioEngineState = 'RSE'
    $entry.Delete()
    $writer = [IO.StreamWriter]::new($archive.CreateEntry('Content/score.gpif').Open(), [Text.UTF8Encoding]::new($false))
    try { $writer.Write($gpif.OuterXml) } finally { $writer.Dispose() }
} finally { $archive.Dispose() }

$saved = @{}
foreach ($name in @('QT_PLUGIN_PATH','QT_QPA_GENERIC_PLUGINS','GPVST3_DATA_DIR','GPVST3_ENABLE_P2_HOOK','GPVST3_ENABLE_P2_EFFECT','GPVST3_RUNTIME_VST3','GPVST3_TOTAL_BYPASS','GPVST3_FORCE_P3_ERROR','GPVST3_ENABLE_P4_INPUT','GPVST3_P4_ROUTE','GPMCP_DATA_DIR','GPMCP_SESSION_FILE','GPMCP_BACKGROUND','GPMCP_DEVELOPMENT','TEMP','TMP')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$process = $null
$session = $null
try {
    Remove-Item Env:QT_PLUGIN_PATH,Env:QT_QPA_GENERIC_PLUGINS -ErrorAction SilentlyContinue
    $env:GPVST3_DATA_DIR = $run
    $env:GPVST3_ENABLE_P2_HOOK = '1'
    $env:GPVST3_ENABLE_P2_EFFECT = '1'
    if ($EnableP4) {
        $env:GPVST3_ENABLE_P4_INPUT = '1'
        $env:GPVST3_P4_ROUTE = $P4Route
    } else {
        Remove-Item Env:GPVST3_ENABLE_P4_INPUT,Env:GPVST3_P4_ROUTE -ErrorAction SilentlyContinue
    }
    if ($ExpectP3Fallback) { $env:GPVST3_FORCE_P3_ERROR = '1' }
    else { Remove-Item Env:GPVST3_FORCE_P3_ERROR -ErrorAction SilentlyContinue }
    if ($ExpectP3TotalBypass) { $env:GPVST3_TOTAL_BYPASS = '1' }
    else { Remove-Item Env:GPVST3_TOTAL_BYPASS -ErrorAction SilentlyContinue }
    $programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    $env:GPVST3_RUNTIME_VST3 = Join-Path $programFiles 'Common Files/VST3/ParametricOD.vst3'
    $env:GPMCP_DATA_DIR = Join-Path $run 'mcp'
    $env:GPMCP_SESSION_FILE = Join-Path $run 'mcp/native-session.json'
    $env:GPMCP_BACKGROUND = '1'
    $env:GPMCP_DEVELOPMENT = '1'
    $env:TEMP = $run
    $env:TMP = $run
    $process = Start-Process -FilePath (Join-Path $hostCopy 'GuitarPro.exe') -WorkingDirectory $hostCopy -WindowStyle Hidden -PassThru
    $statusPath = Join-Path $run 'status.json'
    $sessionPath = $env:GPMCP_SESSION_FILE
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if ($process.HasExited) { throw "Guitar Pro exited before P2 runtime observation started ($($process.ExitCode))." }
    } while ((-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) -and [DateTime]::UtcNow -lt $deadline)
    if (-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) { throw 'P2 runtime status/session was not published.' }

    . $mcpClient
    $session = New-McpSession -SessionFile $sessionPath
    $opened = Invoke-McpTool $session gp_open @{path=$fixture}
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $operation = Invoke-McpTool $session gp_operation @{request=$opened.request}
    } while ($operation.operation.status -notin @('opened','error','cancelled') -and [DateTime]::UtcNow -lt $deadline)
    if ($operation.operation.status -ne 'opened') { throw "Fixture did not open: $($operation | ConvertTo-Json -Depth 8 -Compress)" }
    $document = $operation.operation.document
    Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
    Invoke-McpTool $session gp_playback @{operation='play';document=$document} | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        $playback = Invoke-McpTool $session gp_playback @{document=$document}
    } while (-not $playback.playing -and [DateTime]::UtcNow -lt $deadline)
    if (-not $playback.playing) { throw 'Fixture playback did not start.' }
    Start-Sleep -Seconds 2
    Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null
    Start-Sleep -Milliseconds 400
    $observationPath = Join-Path $run 'p2-observation.json'
    if (-not (Test-Path -LiteralPath $observationPath)) { throw 'P2 observation snapshot was not written.' }
    $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
    $hook = $observation.gp_hook
    if (-not $hook.master_process.call_observed -or -not $hook.effects_chain_processDSP.call_observed) { throw "P2 process entry was not observed: $($hook | ConvertTo-Json -Depth 8 -Compress)" }
    if ($hook.master_process.frame_count -le 0 -or $hook.master_process.channel_count -le 0 -or
        $hook.master_process.sample_rate -le 0 -or -not $hook.master_process.buffer_write_observed) {
        throw "P2 block metadata or host buffer mutation was not observed: $($hook | ConvertTo-Json -Depth 8 -Compress)"
    }
    if (-not $hook.runtime_effect_enabled -or -not $hook.runtime_processor_ready) {
        throw "P2 runtime VST3 processor was not ready: $($hook | ConvertTo-Json -Depth 8 -Compress)"
    }
    if ($EnableP4) {
        if (-not $hook.input_route_enabled -or -not $hook.input_processor_ready -or
            $hook.input_route -ne $P4Route -or -not $hook.audio_layer_input_level_accessor_found) {
            throw "P4 input route/AudioLayer level monitor was not ready: $($hook | ConvertTo-Json -Depth 8 -Compress)"
        }
    }
    if ($ExpectP3Fallback) {
        if (-not $hook.chain_faulted -or -not $hook.total_bypass -or
            $hook.chain_error_blocks -le 0 -or $hook.chain_fallback_blocks -le 0) {
            throw "P3 detectable error did not enter bypass fallback: $($hook | ConvertTo-Json -Depth 8 -Compress)"
        }
    } elseif ($ExpectP3TotalBypass) {
        if (-not $hook.total_bypass -or $hook.chain_faulted -or
            $hook.chain_bypass_blocks -le 0 -or $hook.chain_processed_blocks -ne 0) {
            throw "P3 total bypass did not pass through without processing: $($hook | ConvertTo-Json -Depth 8 -Compress)"
        }
    } elseif (-not $hook.runtime_process_observed -or -not $hook.runtime_buffer_write_observed) {
        throw "P2 runtime VST3 effect processing was not observed: $($hook | ConvertTo-Json -Depth 8 -Compress)"
    }
    if ((-not $ExpectP3Fallback -and -not $ExpectP3TotalBypass -and ($hook.total_bypass -or $hook.chain_faulted)) -or
        $hook.chain_prepared_slots -lt 2 -or
        $hook.chain_process_blocks -le 0 -or
        ((-not $ExpectP3Fallback -and -not $ExpectP3TotalBypass) -and $hook.chain_processed_blocks -le 0) -or
        ((-not $ExpectP3Fallback -and -not $ExpectP3TotalBypass) -and $hook.chain_switch_count -lt 2) -or
        -not $hook.reconfiguration_validated -or
        $hook.reconfiguration_passed -ne 10 -or $hook.reconfiguration_failed -ne 0) {
        throw "P3 chain safety/reconfiguration validation failed: $($hook | ConvertTo-Json -Depth 8 -Compress)"
    }
    @{status='passed';playback=$playback;gp_hook=$hook;host_sha256=(Get-FileHash -LiteralPath (Join-Path $hostCopy 'GuitarPro.exe')).Hash} |
        ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $run 'verification.json')
    Write-Output "PASS: P2 runtime Master/processDSP and VST3 effect processing. Evidence: $run"
} finally {
    if ($session) { try { Invoke-McpTool $session gp_playback @{operation='stop'} -AllowError | Out-Null } catch {} ; try { Close-McpSession $session } catch {} }
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force; $process.WaitForExit(5000) | Out-Null }
        else { $process.WaitForExit(5000) | Out-Null }
        $process.Dispose()
    }
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    if (-not $KeepHost) {
        $full = (Resolve-Path -LiteralPath $hostCopy -ErrorAction SilentlyContinue).Path
        if ($full) {
            $toolsRoot = (Resolve-Path -LiteralPath (Join-Path $root '.tools')).Path.TrimEnd('\') + '\'
            if (-not $full.StartsWith($toolsRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to remove a path outside the project .tools directory: $full"
            }
            $removed = $false
            for ($attempt = 0; $attempt -lt 30 -and -not $removed; ++$attempt) {
                try {
                    [IO.Directory]::Delete($full, $true)
                    $removed = $true
                } catch {
                    Start-Sleep -Seconds 1
                }
            }
            if (-not $removed) { Write-Warning "Runtime host copy could not be removed; inspect and delete when no process holds it: $full" }
        }
    }
}
