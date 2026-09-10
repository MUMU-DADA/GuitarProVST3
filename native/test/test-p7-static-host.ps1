param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = '',
    [switch]$ObserveProgress,
    [switch]$ExpectFailure,
    [switch]$ExitDuringScan
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
$run = Join-Path $root ('artifacts/p7-static-host-' + [guid]::NewGuid().ToString('N'))
$plugins = Join-Path $run 'Test Plugins'
$data = Join-Path $run 'data'
New-Item -ItemType Directory -Force -Path $run,$plugins,$data | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$marker = Join-Path $run 'marker.dll'
& cl /nologo /LD /MD /O2 /utf-8 (Join-Path $PSScriptRoot 'p7_scan_marker.cpp') "/Fo$run/" "/Fe$marker" /link "/IMPLIB:$run/marker.lib"
if ($LASTEXITCODE) { throw 'Scan marker compilation failed.' }
foreach ($name in @('Marker A','Marker B')) {
    $bundle = Join-Path $plugins "$name.vst3"
    New-Item -ItemType Directory -Force -Path "$bundle/Contents/x86_64-win" | Out-Null
    Copy-Item -LiteralPath $marker -Destination "$bundle/Contents/x86_64-win/$name.vst3"
}
. (Join-Path $PSScriptRoot 'host-session.ps1')
. (Join-Path $McpRoot 'native/mcp-client.ps1')
$before = Get-Gpvst3HostSnapshot $HostDirectory
$fixture = Join-Path $run 'fixture.gp'
$fixtureSource = Join-Path $McpRoot 'native/testdata/minimal.gp'
if (-not (Test-Path -LiteralPath $fixtureSource)) { $fixtureSource = Join-Path $McpRoot 'test/testdata/minimal.gp' }
Copy-Item -LiteralPath $fixtureSource -Destination $fixture
$entryLog = Join-Path $run 'entries.log'
$results = @()
$network = @()
try {
    foreach ($phase in $(if ($ExitDuringScan) { @('cold') } else { @('cold','restart') })) {
        $process = $null; $session = $null
        $phaseRun = Join-Path $run $phase
        try {
            $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $phaseRun -McpRoot $McpRoot -Environment @{
                GPVST3_DATA_DIR=$data; GPVST3_VST3_ROOT=$(if ($ExpectFailure) { '//invalid-p7-root/vst3' } else { $plugins }); GPVST3_TEST_ENTRY_LOG=$entryLog
            }
            $statusPath = Join-Path $data 'status.json'
            $sessionPath = Join-Path $phaseRun 'mcp/native-session.json'
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            do {
                $connections = @(Get-NetTCPConnection -OwningProcess $process.Id -ErrorAction SilentlyContinue |
                    Select-Object OwningProcess,LocalAddress,LocalPort,RemoteAddress,RemotePort,State)
                $network += @{phase=$phase;pid=$process.Id;utc=[DateTime]::UtcNow.ToString('o');connections=$connections}
                $status = if (Test-Path -LiteralPath $statusPath) { Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json } else { $null }
                if ($status.pid -eq $process.Id -and ($ObserveProgress -or -not $status.vst3_host.scan_pending) -and (Test-Path -LiteralPath $sessionPath)) { break }
                Start-Sleep -Milliseconds 50
            } while ([DateTime]::UtcNow -lt $deadline)
            if ($status.pid -ne $process.Id -or (-not $ObserveProgress -and $status.vst3_host.scan_pending)) { throw 'Static host scan did not terminate.' }
            $identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
            if (Test-Path -LiteralPath $entryLog) { throw 'Cold/restart scan executed marker code.' }
            if (-not $ExpectFailure -and -not $ObserveProgress -and ($status.vst3_host.modules_loaded -ne 0 -or @($status.vst3_catalog).Count -ne 2)) { throw 'Unexpected static catalog.' }
            if (-not $ExpectFailure -and -not $ObserveProgress -and $phase -eq 'restart' -and (-not $status.vst3_host.cache_hit -or $status.vst3_host.cache_reused -ne 2)) { throw 'Second GP process did not reuse cache.' }
            $session = New-McpSession -SessionFile $sessionPath
            $open = Invoke-McpTool $session gp_open @{path=$fixture}
            $openDeadline = [DateTime]::UtcNow.AddSeconds(10)
            do {
                $operation = Invoke-McpTool $session gp_operation @{request=$open.request}
                if ($operation.operation.document) { break }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $openDeadline)
            Invoke-McpTool $session gp_activate @{document=$operation.operation.document} | Out-Null
            Start-Sleep -Milliseconds 600
            $entry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
            if ($ExpectFailure) {
                if ($status.vst3_host.status -ne 'scan_failed' -or @($entry.objects)[0].properties.text -notmatch '扫描失败，点击重试') { throw 'Failure button and scan terminal state are missing.' }
                $oldGeneration = $status.vst3_host.scan_generation
                Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=@($entry.objects)[0].id} | Out-Null
                Start-Sleep -Milliseconds 300
                $retry = (Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json).vst3_host
                if ($retry.scan_generation -le $oldGeneration -or $retry.status -ne 'scan_failed') { throw 'Failure retry did not run and terminate.' }
                $results += @{phase=$phase;identity=$identity;scan=$status.vst3_host;failure_entry=$entry;retry=$retry}
                continue
            }
            $progress = $null
            if ($ObserveProgress) {
                $progress = @{entry=$entry;scan=(Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json).vst3_host}
                if (-not $progress.scan.scan_pending -or (@($entry.objects)[0].properties.text -notmatch '扫描|更新')) { throw 'MCP did not observe immediate pending button feedback.' }
                if ($ExitDuringScan) {
                    $results += @{phase=$phase;identity=$identity;progress=$progress;exit_during_scan=$true}
                    continue
                }
                $activeGeneration = $progress.scan.scan_generation
                for ($click=0;$click -lt 3;$click++) {
                    $entry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
                    Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=@($entry.objects)[0].id} | Out-Null
                }
                if ((Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json).vst3_host.scan_generation -ne $activeGeneration) { throw 'Repeated clicks started overlapping scans.' }
                $deadline = [DateTime]::UtcNow.AddSeconds(20)
                do {
                    Start-Sleep -Milliseconds 100
                    $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
                } while ($status.vst3_host.scan_pending -and [DateTime]::UtcNow -lt $deadline)
                if ($status.vst3_host.scan_pending) { throw 'Delayed static scan failed to finish.' }
                $entry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
                if (@($entry.objects)[0].properties.text -ne 'VST3') { throw 'Scan button did not return to terminal text.' }
            }
            $clock.Restart()
            Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=@($entry.objects)[0].id} | Out-Null
            $list = Invoke-McpTool $session gp_objects @{query='gpvst3Identify_';limit=10}
            $openMs = $clock.ElapsedMilliseconds
            if (@($list.objects).Count -ne 2 -or @($list.objects | Where-Object { -not $_.properties.enabled }).Count) { throw 'Cached candidate list is not operable.' }
            if ($phase -eq 'restart' -and $openMs -gt 500) { throw "Cache list exceeded 500 ms: $openMs" }
            $generation = (Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json).vst3_host.scan_generation
            # The entry remains usable across repeated requests. Static refresh never executes candidates.
            for ($i=0;$i -lt 3;$i++) {
                $entry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
                Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=@($entry.objects)[0].id} | Out-Null
            }
            Start-Sleep -Milliseconds 300
            if (Test-Path -LiteralPath $entryLog) { throw 'Manual refresh executed marker code.' }
            $record = [ordered]@{phase=$phase;identity=$identity;scan=$status.vst3_host;list_open_ms=$openMs;list=$list;entries_during_scan=0;generation=$generation;progress=$progress}
            if ($phase -eq 'restart') {
                $candidate = Invoke-McpTool $session gp_objects @{query='gpvst3Identify_';limit=10}
                $target = @($candidate.objects)[0]
                Invoke-McpTool $session gp_set_property @{snapshot=$candidate.snapshot;id=$target.id;property='checked';value=$true} | Out-Null
                Start-Sleep -Milliseconds 500
                if (-not (Test-Path -LiteralPath $entryLog)) { throw 'Explicit selection did not reach the test factory.' }
                $lines = @(Get-Content -LiteralPath $entryLog -Encoding Unicode)
                $loadedBundles = @($lines | ForEach-Object { ($_ -split "`t")[1] } | Sort-Object -Unique)
                if ($loadedBundles.Count -ne 1 -or ($lines -join '') -notmatch 'GetPluginFactory') { throw 'Selection did not isolate one bundle.' }
                $notice = Invoke-McpTool $session gp_objects @{query='gpvst3Status';limit=10}
                if (($notice | ConvertTo-Json -Depth 10) -notmatch 'null_factory') { throw 'Failed identification did not report factory error.' }
                $entryBytes = (Get-Item -LiteralPath $entryLog).Length
                $entry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
                Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=@($entry.objects)[0].id} | Out-Null
                Start-Sleep -Milliseconds 500
                if ((Get-Item -LiteralPath $entryLog).Length -ne $entryBytes) { throw 'Refresh retried dynamic identification automatically.' }
                $saved = Get-Content -LiteralPath (Join-Path $data 'effect-chain.json') -Raw | ConvertFrom-Json
                if (@($saved.effects | Where-Object enabled).Count) { throw 'Failed marker selection persisted enabled state.' }
                $record.explicit_selection_entries = $lines
                $record.failure_notice = $notice
            }
            $results += $record
        } finally {
            if ($session) {
                try {
                    $deadline = [DateTime]::UtcNow.AddSeconds(20)
                    while (-not $ExitDuringScan -and (Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json).vst3_host.scan_pending -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
                    Request-Gpvst3McpHostExit $session $process
                    Start-Sleep -Milliseconds 500
                } catch { Write-Warning $_.Exception.Message }
                try { Close-McpSession $session } catch {}
            }
            Stop-Gpvst3TestHost $process -RunDirectory $phaseRun
            $shutdown = Get-Content -LiteralPath (Join-Path $phaseRun 'shutdown.json') -Raw | ConvertFrom-Json
            if ($shutdown.forced -or $shutdown.exit_code -ne 0) { throw 'Guitar Pro did not exit cleanly.' }
        }
    }
    @{passed=$true;phases=$results;network_scope='TCP table samples by actual GP PID; third-party attribution uses entry markers, not absence of firewall prompts';network=$network} |
        ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: real GP cold/restart cache, static marker isolation, explicit selection failure and TCP attribution samples. Evidence: $run"
} finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
