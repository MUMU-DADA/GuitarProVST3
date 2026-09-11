param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
$run = Join-Path $root ('artifacts/p8-recognition-host-' + [guid]::NewGuid().ToString('N'))
$data = Join-Path $run 'data'
$plugins = Join-Path $run 'plugins'
$slow = Join-Path $plugins 'A Slow.vst3'
$bin = Join-Path $slow 'Contents/x86_64-win'
New-Item -ItemType Directory -Force -Path $bin,$data | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
& cl /nologo /LD /MD /O2 /utf-8 /DP8_SCAN_DELAY_MS=11000 (Join-Path $PSScriptRoot 'p7_scan_marker.cpp') "/Fo$run/" "/Fe$bin/A Slow.vst3" /link "/IMPLIB:$run/slow.lib"
if ($LASTEXITCODE) { throw 'P8 slow factory fixture compilation failed.' }
Copy-Item -LiteralPath (Join-Path $root '.tools/native/p8-order-test/P8 Order Fixture.vst3') -Destination $plugins -Recurse
. (Join-Path $PSScriptRoot 'host-session.ps1')
. (Join-Path $McpRoot 'native/mcp-client.ps1')
$before = Get-Gpvst3HostSnapshot $HostDirectory
$entryLog = Join-Path $run 'entries.log'
$result = [ordered]@{run=$run;phases=@()}
$process = $null; $session = $null
try {
    foreach ($phase in @('cold','restart','queue','queue-restart')) {
        if ($phase -eq 'queue') {
            # Force actual factory recognition of the fast bundle after the
            # timeout; cold also covers a timeout as the final queued task.
            $data = Join-Path $run 'queue-data'
            New-Item -ItemType Directory -Path $data | Out-Null
            Remove-Item -LiteralPath (Join-Path $plugins 'P8 Order Fixture.vst3/Contents/Resources/moduleinfo.json')
        }
        $phaseRun = Join-Path $run $phase
        $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $phaseRun -McpRoot $McpRoot -Environment @{
            GPVST3_DATA_DIR=$data;GPVST3_VST3_ROOT=$plugins;GPVST3_TEST_ENTRY_LOG=$entryLog;GPVST3_ENABLE_P2_EFFECT='0'
        }
        $statusPath = Join-Path $data 'status.json'
        $sessionPath = Join-Path $phaseRun 'mcp/native-session.json'
        $deadline = [DateTime]::UtcNow.AddSeconds(25)
        do {
            Start-Sleep -Milliseconds 50
            if (Test-Path $statusPath) { $status = Get-Content $statusPath -Raw | ConvertFrom-Json }
        } while (($status.pid -ne $process.Id -or -not (Test-Path $sessionPath)) -and [DateTime]::UtcNow -lt $deadline)
        $identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
        $session = New-McpSession -SessionFile $sessionPath
        $score = Join-Path $phaseRun 'seed.gp'
        Copy-Item -LiteralPath (Join-Path $McpRoot 'test/testdata/minimal.gp') -Destination $score
        $open = Invoke-McpTool $session gp_open @{path=$score}
        do {
            $operation = (Invoke-McpTool $session gp_operation @{request=$open.request}).operation
            if ($operation.status -eq 'opened') { break }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($operation.status -ne 'opened') { throw 'Recognition test score did not open.' }
        Invoke-McpTool $session gp_activate @{document=$operation.document} | Out-Null
        $responses = @()
        do {
            $status = Get-Content $statusPath -Raw | ConvertFrom-Json
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $ui = Invoke-McpTool $session gp_objects @{query='gpvst3';limit=200}
            $clock.Stop()
            $rows = @($ui.objects | Where-Object { $_.object_name -match '^gpvst3(Global)?(Enabled_|Name_|Editor_|Identify_)' })
            if (@($rows | Where-Object { $_.properties.text -like '*A Slow*' -or $_.properties.toolTip -like '*A Slow*' }).Count) {
                throw 'A pending/timed-out factory appeared as an operable plugin row.'
            }
            if ($status.vst3_host.recognition_pending) {
                $responses += @{elapsed_ms=$clock.ElapsedMilliseconds;scan=$status.vst3_host;rows=$rows}
                if ($clock.ElapsedMilliseconds -gt 1000) { throw 'Qt/MCP did not respond during background factory execution.' }
            }
            if (-not $status.vst3_host.scan_pending -and -not $status.vst3_host.recognition_pending -and
                @($status.vst3_catalog | Where-Object recognition_status -eq 'ready').Count -eq 3) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($status.vst3_host.scan_pending -or $status.vst3_host.recognition_pending -or
            @($status.vst3_catalog | Where-Object recognition_status -eq 'ready').Count -ne 3) {
            throw 'Timeout did not advance to the next actual VST3 bundle.'
        }
        if ($status.vst3_host.modules_loaded -ne 0 -or $status.vst3_host.instances_created -ne 0) {
            throw 'Static scan executed code or instantiated a processor.'
        }
        $cache = Get-Content (Join-Path $data 'vst3-catalog-cache.json') -Raw | ConvertFrom-Json
        $modules = @($cache.scopes.PSObject.Properties.Value.modules.PSObject.Properties)
        $timeout = @($modules | Where-Object { $_.Name -like '*A Slow.vst3' })[0].Value
        if ($timeout.recognition_status -ne 'timeout' -or $timeout.recognition_error -ne 'recognition_timeout' -or
            $timeout.recognition_attempts -ne 1 -or $timeout.recognition_deadline_at -le 0) {
            throw 'The real factory timeout was not cached.'
        }
        $entries = @(Get-Content -LiteralPath $entryLog -Encoding Unicode)
        if ($phase -in @('cold','queue')) {
            if ($responses.Count -lt 2 -or $status.vst3_host.recognition_timed_out -ne 1 -or
                $status.vst3_host.recognition_workers_detached -ne 1 -or
                @($entries | Where-Object { $_ -match 'GetPluginFactory' -and ($_ -split "`t")[0] -eq [string]$process.Id }).Count -ne 1) { throw 'No actual timed-out worker/Qt response evidence.' }
            if ($phase -eq 'queue' -and $status.vst3_host.recognition_completed -ne 2) { throw 'The second bundle was not factory-recognized after the timeout.' }
        } elseif (-not $status.vst3_host.cache_hit -or $status.vst3_host.recognition_attempted -ne 0 -or
            @($entries | Where-Object { ($_ -split "`t")[0] -eq [string]$process.Id }).Count) {
            throw 'Restart retried the timed-out bundle or failed to reuse recognition results.'
        }
        $result.phases += @{phase=$phase;identity=$identity;scan=$status.vst3_host;responses=$responses;timeout=$timeout;ui=$ui;entries=$entries}
        $clock = [Diagnostics.Stopwatch]::StartNew()
        Request-Gpvst3McpHostExit $session $process
        try { Close-McpSession $session } catch { }
        $session = $null
        Stop-Gpvst3TestHost $process -RunDirectory $phaseRun
        $process = $null
        $clock.Stop()
        $exit = Get-Content (Join-Path $phaseRun 'shutdown.json') -Raw | ConvertFrom-Json
        if ($exit.forced -or -not $exit.exited -or $exit.exit_code -ne 0) { throw 'Recognition test host did not exit normally.' }
        $result.phases[-1].exit = $exit
        $result.phases[-1].exit_ms = $clock.ElapsedMilliseconds
    }
    $result | ConvertTo-Json -Depth 35 | Set-Content (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P8 actual 11-second factory, Qt responsiveness, timeout hiding, queue continuation, clean exit and cross-process cache. Evidence: $run"
} catch {
    $result.failure = $_.Exception.Message
    $result | ConvertTo-Json -Depth 35 | Set-Content (Join-Path $run 'verification.json') -Encoding UTF8
    throw
} finally {
    if ($session) { try { Request-Gpvst3McpHostExit $session $process; Close-McpSession $session } catch { } }
    try { Stop-Gpvst3TestHost $process -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
