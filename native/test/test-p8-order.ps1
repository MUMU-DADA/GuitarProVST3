param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = '',
    [string]$FixturePath = '',
    [ValidateSet('disabled','input_insert','bus_mix')][string]$P4Route = 'disabled'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not $FixturePath) { $FixturePath = Join-Path $root '.tools/native/p8-order-test/P8 Order Fixture.vst3' }
. (Join-Path $PSScriptRoot 'host-session.ps1')
. (Join-Path $McpRoot 'native/mcp-client.ps1')
$run = Join-Path $root ('artifacts/mcp-p8-order-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$before = Get-Gpvst3HostSnapshot $HostDirectory
$result = [ordered]@{run=$run}
$process = $null; $session = $null; $document = $null
function Json($value) { ConvertTo-Json -InputObject $value -Depth 30 -Compress }
function Wait-Operation($request, $expected) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $operation = (Invoke-McpTool $session gp_operation @{request=$request}).operation
        if ($operation.status -in @($expected,'error','cancelled')) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($operation.status -ne $expected) { throw "Operation failed: $(Json $operation)" }
    $operation
}
function Trigger([string]$name) {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $query = Invoke-McpTool $session gp_objects @{query=$name;limit=20}
        $item = @($query.objects | Where-Object object_name -eq $name)[0]
        if ($item) {
            Invoke-McpTool $session gp_trigger @{snapshot=$query.snapshot;id=$item.id} | Out-Null
            return
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Control missing: $name"
}
function Set-Property([string]$name, [string]$property, $value) {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $query = Invoke-McpTool $session gp_objects @{query=$name;limit=20}
        $item = @($query.objects | Where-Object object_name -eq $name)[0]
        if (-not $item) { Start-Sleep -Milliseconds 150; continue }
        try { Invoke-McpTool $session gp_set_property @{snapshot=$query.snapshot;id=$item.id;property=$property;value=$value} | Out-Null } catch { Start-Sleep -Milliseconds 150; continue }
        Start-Sleep -Milliseconds 250
        $readback = Invoke-McpTool $session gp_objects @{query=$name;limit=20}
        $current = @($readback.objects | Where-Object object_name -eq $name)[0]
        if ($current -and $current.properties.$property -eq $value) { return }
    } while ([DateTime]::UtcNow -lt $deadline)
    $notice = Invoke-McpTool $session gp_objects @{query='gpvst3Status';limit=20}
    throw "Control write was not retained ($name/$property): $(Json @{control=$current;status=$notice})"
}
function Prefix([string]$scope) { if ($scope -eq 'global') { 'gpvst3Global' } else { 'gpvst3' } }
function Read-Processor([string]$scope, $candidate) {
    Trigger ((Prefix $scope) + 'Editor_' + $candidate.class_id)
    $expectedOperation = [array]::IndexOf(@('P8 A Offset','P8 B Gain','P8 C Offset'), $candidate.name)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 150
        $query = Invoke-McpTool $session gp_objects @{query='gpvst3TestProcessor';limit=10}
        $data = $query.objects[0].properties.text | ConvertFrom-Json
        if ($data.instance -and $data.operation -eq $expectedOperation) { break }
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $data.instance -or $data.operation -ne $expectedOperation) { throw "Requested processor editor did not become current: $(Json $data)" }
    $data
}
function Read-Order([string]$scope) {
    $chain = Get-Content -LiteralPath (Join-Path $run 'effect-chain.json') -Raw | ConvertFrom-Json
    $effects = if ($scope -eq 'global') { $chain.global.effects } else {
        $score = @($chain.scores.PSObject.Properties | Where-Object { [IO.Path]::GetFullPath($_.Name) -ieq [IO.Path]::GetFullPath($scorePath) })[0].Value
        @($score.tracks.PSObject.Properties.Value | Where-Object { $_.present -and $_.track_index -eq 0 })[0].effects
    }
    @($effects | Where-Object enabled | ForEach-Object name) -join '|'
}
function Assert-AudioOrder([string]$scope, [int[]]$order) {
    Invoke-McpTool $session gp_playback @{operation='play';document=$document} | Out-Null
    Start-Sleep -Milliseconds 1200
    Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null
    Start-Sleep -Milliseconds 300
    $measurements = @($order | ForEach-Object { Read-Processor $scope $candidates[$_] })
    for ($i=0; $i -lt $measurements.Count; $i++) {
        $current = $measurements[$i]
        if ($current.blocks -lt 1) { throw "No real process calls: $(Json $current)" }
        $expected = if ($order[$i] -eq 1) { $current.first_input * 0.5 } else { $current.first_input + $(if ($order[$i] -eq 0) {0.125} else {0.25}) }
        if ([Math]::Abs($current.first_output - $expected) -gt 0.000001) { throw "Fixture operation mismatch ($scope): $(Json $measurements)" }
        if ($i -gt 0 -and ($measurements[$i-1].output_hash -ne $current.input_hash -or $measurements[$i-1].last_sequence -ge $current.last_sequence)) {
            throw "$scope audio buffers do not follow the displayed order: $(Json $measurements)"
        }
    }
    $offset = if ($order[0] -eq 0) {0.3125} else {0.1875}
    if ([Math]::Abs($measurements[-1].first_output - ($measurements[0].first_input * 0.5 + $offset)) -gt 0.000001) {
        throw "$scope reordered audio did not produce its expected noncommutative result."
    }
    $savedOrder = Read-Order $scope
    $expectedOrder = @($order | ForEach-Object { $candidates[$_].name }) -join '|'
    if ($savedOrder -ne $expectedOrder) { throw "Persisted $scope order differs: $savedOrder" }
    @{scope=$scope;order=$savedOrder;processors=$measurements}
}
try {
    $environment = @{GPVST3_ENABLE_P2_EFFECT='0';GPVST3_VST3_ROOT=$FixturePath;GPVST3_RUNTIME_VST3=$FixturePath}
    if ($P4Route -ne 'disabled') { $environment.GPVST3_ENABLE_P4_INPUT='1'; $environment.GPVST3_P4_ROUTE=$P4Route }
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -McpRoot $McpRoot -Environment $environment
    $statusPath = Join-Path $run 'status.json'; $sessionPath = Join-Path $run 'mcp/native-session.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        Start-Sleep -Milliseconds 200
        if (Test-Path -LiteralPath $statusPath) { $startup = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json }
    } while ((-not (Test-Path -LiteralPath $sessionPath) -or @($startup.vst3_catalog | Where-Object { $_.class_id -and $_.recognition_status -eq 'ready' }).Count -ne 3) -and [DateTime]::UtcNow -lt $deadline)
    $result.identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
    $candidates = @($startup.vst3_catalog | Where-Object recognition_status -eq 'ready' | Sort-Object name)
    if ($candidates.Count -ne 3) { throw 'Order fixture did not identify all three audio classes in the background.' }
    $result.catalog = $candidates
    $session = New-McpSession -SessionFile $sessionPath
    $seed = Join-Path $run 'seed.gp'
    Copy-Item -LiteralPath (Join-Path $McpRoot 'test/testdata/minimal.gp') -Destination $seed
    Wait-Operation (Invoke-McpTool $session gp_open @{path=$seed}).request 'opened' | Out-Null
    $document = (Wait-Operation (Invoke-McpTool $session gp_new @{template='Steel Guitar'}).request 'created').document
    Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
    Wait-Operation (Invoke-McpTool $session gp_insert_tab @{document=$document;track=0;string=0;bar=0;text='0-2-5-7';mode='replace';denominator=4}).request 'applied' | Out-Null
    $scorePath = Join-Path $run 'order.gp'
    Wait-Operation (Invoke-McpTool $session gp_save_as @{document=$document;path=$scorePath}).request 'saved' | Out-Null
    Invoke-McpTool $session gp_cursor @{document=$document;axis='track';index=0} | Out-Null
    Start-Sleep -Milliseconds 700
    Invoke-McpTool $session gp_playback @{operation='set_loop';document=$document;enabled=$true} | Out-Null
    $result.orders = @()
    foreach ($scope in @('track','global')) {
        foreach ($candidate in $candidates) { Set-Property ((Prefix $scope) + 'Enabled_' + $candidate.class_id) 'checked' $true }
        $abc = Assert-AudioOrder $scope @(0,1,2)
        $result.orders += $abc
        $listName = if ($scope -eq 'global') {'gpvst3GlobalChainList'} else {'gpvst3TrackChainList'}
        Set-Property $listName 'currentRow' 2
        Trigger ((Prefix $scope) + 'MoveUp')
        Start-Sleep -Milliseconds 300
        Trigger ((Prefix $scope) + 'MoveUp')
        Start-Sleep -Milliseconds 300
        $cab = Assert-AudioOrder $scope @(2,0,1)
        $result.orders += $cab
        foreach ($processor in $cab.processors) {
            if ($processor.instance -notin @($abc.processors.instance)) { throw "$scope reorder rebuilt an existing processor." }
        }
        Trigger 'gpvst3SoundEffectChainButton'
        Start-Sleep -Seconds 1
        if ((Read-Order $scope) -ne $cab.order) { throw 'Refresh lost the user order.' }
        Set-Property ((Prefix $scope) + 'Enabled_' + $candidates[0].class_id) 'checked' $false
        Set-Property ((Prefix $scope) + 'Enabled_' + $candidates[0].class_id) 'checked' $true
        if ((Read-Order $scope) -ne (@($candidates[2].name,$candidates[1].name,$candidates[0].name) -join '|')) { throw 'Re-enabled effect was not appended after the remaining active effects.' }
        Set-Property $listName 'currentRow' 2
        Trigger ((Prefix $scope) + 'MoveUp')
        Start-Sleep -Milliseconds 250
    }
    Request-Gpvst3McpHostExit $session $process
    try { Close-McpSession $session } catch {}
    $session = $null
    $firstRun = Join-Path $run 'before-restart'; New-Item -ItemType Directory -Path $firstRun | Out-Null
    Stop-Gpvst3TestHost $process -RunDirectory $firstRun; $process = $null
    if ((Get-Content (Join-Path $firstRun 'shutdown.json') -Raw | ConvertFrom-Json).forced) { throw 'Order test first process did not exit normally.' }
    $restartRun = Join-Path $run 'restart'; $environment.GPVST3_DATA_DIR = $run
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $restartRun -McpRoot $McpRoot -Environment $environment
    $sessionPath = Join-Path $restartRun 'mcp/native-session.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 200
        $restartStatus = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    } while ((-not (Test-Path $sessionPath) -or $restartStatus.pid -ne $process.Id) -and [DateTime]::UtcNow -lt $deadline)
    $result.restart_identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
    $session = New-McpSession -SessionFile $sessionPath
    $document = (Wait-Operation (Invoke-McpTool $session gp_open @{path=$scorePath}).request 'opened').document
    Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
    $restartSaved = Get-Content -LiteralPath (Join-Path $run 'effect-chain.json') -Raw | ConvertFrom-Json
    $restartEffects = @($restartSaved.global.effects)
    foreach ($score in $restartSaved.scores.PSObject.Properties.Value) {
        foreach ($track in $score.tracks.PSObject.Properties.Value) { $restartEffects += @($track.effects) }
    }
    if (@($restartEffects | Where-Object enabled).Count -ne 0 -or
        $restartStatus.gp_hook.runtime_processor_ready -or $restartStatus.gp_hook.runtime_effect_instances -ne 0) {
        throw 'Restart automatically enabled a persisted VST3 processor.'
    }
    $result.restart_disabled_effects = $restartEffects.Count
    # Startup intentionally leaves every persisted VST3 entry disabled. Re
    # enable the chains explicitly before checking post-restart processing.
    foreach ($scope in @('track','global')) {
        foreach ($index in @(2,0,1)) {
            $candidate = $candidates[$index]
            Set-Property ((Prefix $scope) + 'Enabled_' + $candidate.class_id) 'checked' $true
        }
    }
    Start-Sleep -Seconds 1
    $result.restart_orders = @(Assert-AudioOrder 'track' @(2,0,1); Assert-AudioOrder 'global' @(2,0,1))
    $result.observation = Get-Content -LiteralPath (Join-Path $run 'p2-observation.json') -Raw | ConvertFrom-Json
    if ($P4Route -ne 'disabled') {
        $hook = $result.observation.gp_hook
        if ($hook.input_route -ne $P4Route -or $hook.input_after_original_blocks -lt 1 -or
            $hook.input_post_original_hash -eq $hook.input_post_route_hash -or $hook.input_error_blocks -gt 0 -or
            $hook.global_chain_process_blocks -lt 1 -or -not $hook.track_runtime_processed -or
            -not $hook.input_order_samples_observed) {
            throw 'P4 did not process/write after the original PortAudio callback alongside independent global/track chains.'
        }
        # P4 loads fixture A (+0.125). This tuple is copied from one real device
        # callback after both scopes have processed and GP output is non-silent.
        $expected = $hook.input_order_capture_sample + 0.125
        if ($P4Route -eq 'bus_mix') { $expected += $hook.input_order_generated_sample }
        if ([Math]::Abs($hook.input_order_output_sample - $expected) -gt 0.000001) {
            throw "P4 $P4Route output does not follow the capture/generated/process order: $(Json $hook)"
        }
        $result.p4_route = $P4Route
    }
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P8 actual track/global ABC to CAB audio order, instance reuse, refresh, re-enable and process restart. Evidence: $run"
}
catch {
    $result.failure = $_.Exception.Message; $result.failure_stack = $_.ScriptStackTrace
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    throw
}
finally {
    if ($session) {
        try { Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null; Request-Gpvst3McpHostExit $session $process } catch { Write-Warning $_.Exception.Message }
        try { Close-McpSession $session } catch {}
    }
    try { Stop-Gpvst3TestHost $process -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
