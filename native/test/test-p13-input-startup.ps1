param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = '',
    [string]$FixturePath = '',
    [string]$OutputRoot = ''
)

# Real-host production regression. Never writes device/listener controls and
# never attaches to an existing process. The only edited settings are in the
# new test-local input sidecar and its gain fixture editor.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p13-release/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not $FixturePath) { $FixturePath = Join-Path $root '.tools/native/p12-gain-fixture/P7 Gain Fixture.vst3' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root 'artifacts' }
$PluginPath = (Resolve-Path -LiteralPath $PluginPath).Path
$FixturePath = (Resolve-Path -LiteralPath $FixturePath).Path
$hostExe = Join-Path $HostDirectory 'GuitarPro.exe'
$seedSource = Join-Path $McpRoot 'test/testdata/minimal.gp'
foreach ($path in @($hostExe, $seedSource, (Join-Path $McpRoot 'native/mcp-client.ps1'),
        (Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing startup regression prerequisite: $path" }
}
if ((Get-Item -LiteralPath $hostExe).VersionInfo.FileVersion -ne '8.1.1.17') { throw 'This regression requires Guitar Pro 8.1.1.17.' }
if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($PluginPath)).Contains('GPVST3_P13_EXPERIMENTAL_DIAGNOSTICS_NOT_FOR_RELEASE')) {
    throw 'Input startup regression requires a production DLL.'
}
if (@(Get-Process -Name GuitarPro -ErrorAction SilentlyContinue).Count) {
    throw 'Guitar Pro is already running; the startup regression does not attach to or close user processes.'
}
. (Join-Path $PSScriptRoot 'host-session.ps1')
. (Join-Path $McpRoot 'native/mcp-client.ps1')
$run = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('p13-input-startup-' + [guid]::NewGuid().ToString('N'))
$dataDirectory = Join-Path $run 'data'
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
$statusPath = Join-Path $dataDirectory 'status.json'
$observationPath = Join-Path $dataDirectory 'p2-observation.json'
$chainPath = Join-Path $dataDirectory 'effect-chain.json'
$scoreCopy = Join-Path $run 'startup.gp'
Copy-Item -LiteralPath $seedSource -Destination $scoreCopy
$seedHash = (Get-FileHash -LiteralPath $seedSource).Hash
$classId = '40302010605080701122334455667788'
$savedModule = $FixturePath.Replace('\', '/').ToLowerInvariant()
$initialState = [Convert]::ToBase64String([BitConverter]::GetBytes([double]0.25))
@{
    schema=2;score_id='unspecified';track=0;bus='master';effects=@();global=@{effects=@()};scores=@{}
    input=@{monitor_mode='low_latency_overlay';input_gain=0.35;effects=@(@{
        module=$savedModule;class_id=$classId;name='P7 Gain Fixture';vendor='Test';order=0
        entry_id=($savedModule + "`n" + $classId);identified=$true;configured=$true
        enabled=$true;bypass=$false;component_state=$initialState;controller_state=$initialState
    })}
} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $chainPath -Encoding UTF8
$result = [ordered]@{schema=1;status='running';run=$run;runs=@();cleanup_errors=@();
    scope='production input cold startup, restart and saved Off; current ASIO 192000 Hz / 64 frames; no physical audio source required';
    input_switch_writes=0;device_or_native_listener_writes=0}
$before = Get-Gpvst3HostSnapshot $HostDirectory
$session = $null; $process = $null; $document = $null; $currentRun = $null

function Read-StartupJson([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    for ($attempt=0; $attempt -lt 5; ++$attempt) {
        try { return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json }
        catch { if ($attempt -eq 4) { throw }; Start-Sleep -Milliseconds 50 }
    }
}
function Wait-StartupOperation([string]$Request, [string]$Expected) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $operation = (Invoke-McpTool $session gp_operation @{request=$Request}).operation
        if ($operation.status -eq $Expected) { return $operation }
        if ($operation.status -in @('error','cancelled')) { throw "Operation failed: $($operation | ConvertTo-Json -Compress)" }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Operation did not reach ${Expected}: $Request"
}
function Get-StartupControl([string]$Name) {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $query = Invoke-McpTool $session gp_objects @{query=$Name;limit=20}
        $items = @($query.objects | Where-Object object_name -CEQ $Name)
        if ($items.Count -eq 1) { return [pscustomobject]@{snapshot=$query.snapshot;control=$items[0]} }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Expected one $Name control; found $($items.Count)."
}
function Get-StartupListener {
    $query = Invoke-McpTool $session gp_actions @{query='actionActivatedLineIn';limit=20}
    $items = @($query.objects | Where-Object object_name -CEQ 'actionActivatedLineIn')
    if ($items.Count -ne 1 -or $items[0].checked -isnot [bool]) { throw 'Cannot read the native-listener checked state.' }
    return [bool]$items[0].checked
}
function Assert-StartupActive($Monitor, [double]$Gain) {
    if (-not $Monitor -or $Monitor.state -ne 'active' -or $Monitor.mode -ne 'low_latency_overlay' -or
        $Monitor.gain -ne $Gain -or -not $Monitor.configuration_validated -or
        $Monitor.sample_rate -ne 192000 -or $Monitor.driver_buffer_frames -ne 64 -or
        $Monitor.buffer_frames -ne 64 -or $Monitor.process_frames -ne 64 -or
        -not $Monitor.input_native_effect_bypass -or [uint64]$Monitor.processed_blocks -eq 0 -or
        [uint64]$Monitor.error_blocks -ne 0 -or $Monitor.callback_fault_stage -ne 0 -or $Monitor.error) {
        throw "Input startup is not healthy at ASIO 192000/64: $($Monitor | ConvertTo-Json -Compress)"
    }
}
function Wait-StartupActive([double]$Gain) {
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        $process.Refresh()
        if ($process.HasExited) { throw 'Test host exited while waiting for automatic input startup.' }
        $monitor = (Read-StartupJson $observationPath).gp_hook.input_monitor
        if ($monitor.state -eq 'active' -and $monitor.gain -eq $Gain -and [uint64]$monitor.processed_blocks -gt 0) {
            Assert-StartupActive $monitor $Gain
            return $monitor
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Automatic input startup did not become active: $($monitor | ConvertTo-Json -Compress)"
}
function Wait-StartupOff([switch]$FreshProcess) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $process.Refresh()
        if ($process.HasExited) { throw 'Test host exited while waiting for saved Off.' }
        $monitor = (Read-StartupJson $observationPath).gp_hook.input_monitor
        if ($monitor.state -eq 'off' -and $monitor.mode -eq 'off' -and $monitor.gain -eq 0.55 -and
            $monitor.input_native_effect_bypass -eq $false -and $monitor.configuration_validated -eq $false) {
            if ([uint64]$monitor.error_blocks -ne 0 -or $monitor.callback_fault_stage -ne 0 -or $monitor.error -or
                ($FreshProcess -and [uint64]$monitor.processed_blocks -ne 0)) {
                throw "Saved Off acquired processing/error evidence: $($monitor | ConvertTo-Json -Compress)"
            }
            return $monitor
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Input did not settle to saved Off: $($monitor | ConvertTo-Json -Compress)"
}
function Read-StartupFixture([double]$ExpectedGain, [uint64]$PreviousBlocks = 0, [switch]$Dormant) {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $query = Invoke-McpTool $session gp_objects @{query='gpvst3TestProcessor';limit=10}
        $items = @($query.objects | Where-Object { $_.object_name -ceq 'gpvst3TestProcessor' -and $_.properties.text })
        if ($items.Count -eq 1) {
            $fixture = $items[0].properties.text | ConvertFrom-Json
            if ($fixture.instance -and $fixture.gain -eq $ExpectedGain -and
                (($Dormant -and [uint64]$fixture.blocks -eq 0) -or
                 (-not $Dormant -and $fixture.sample_rate -eq 192000 -and [uint64]$fixture.blocks -gt $PreviousBlocks))) { return $fixture }
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Fixture state/process evidence is missing: $($fixture | ConvertTo-Json -Compress)"
}
function Assert-StartupSaved([double]$MonitorGain, [double]$FixtureGain, [string]$Mode = 'low_latency_overlay') {
    $chain = Read-StartupJson $chainPath
    $effects = @($chain.input.effects)
    if ($chain.input.monitor_mode -ne $Mode -or $chain.input.input_gain -ne $MonitorGain -or
        $effects.Count -ne 1 -or -not $effects[0].enabled -or $effects[0].bypass -or
        $effects[0].class_id -cne $classId -or [IO.Path]::GetFullPath($effects[0].module) -ine $FixturePath) {
        throw 'Persisted input mode, gain or selected fixture changed unexpectedly.'
    }
    foreach ($field in @('component_state','controller_state')) {
        $bytes = [Convert]::FromBase64String($effects[0].$field)
        if ($bytes.Length -ne 8 -or [BitConverter]::ToDouble($bytes,0) -ne $FixtureGain) {
            throw "Persisted fixture $field does not contain gain $FixtureGain."
        }
    }
    if (@($chain.global.effects).Count -ne 0) { throw 'Input startup unexpectedly modified the global chain.' }
    return $chain.input
}
function Close-StartupHost {
    if (-not $process) { return }
    if ($session) {
        try {
            if ($document) {
                $closed = Invoke-McpTool $session gp_close @{document=$document;unsaved='discard'}
                Wait-StartupOperation $closed.request 'closed' | Out-Null
            }
            Request-Gpvst3McpHostExit $session $process
        } catch { $result.cleanup_errors += $_.Exception.Message }
        # The MCP transport can disappear during a successful native exit.
        try { Close-McpSession $session } catch { $currentRun.session_close_note=$_.Exception.Message }
        $script:session = $null
    }
    try { Stop-Gpvst3TestHost $process -RunDirectory $currentRun.directory }
    catch { $result.cleanup_errors += $_.Exception.Message }
    $script:process = $null; $script:document = $null
    $currentRun.shutdown = Read-StartupJson (Join-Path $currentRun.directory 'shutdown.json')
    if (-not $currentRun.shutdown -or $currentRun.shutdown.forced -or -not $currentRun.shutdown.exited -or
        $currentRun.shutdown.exit_code -ne 0 -or $currentRun.shutdown.pid -ne $currentRun.identity.pid) {
        $result.cleanup_errors += 'Startup regression host did not exit normally.'
    }
}

try {
    for ($round=0; $round -lt 3; ++$round) {
        if (@(Get-Process -Name GuitarPro -ErrorAction SilentlyContinue).Count) {
            throw 'Another Guitar Pro process is running; restart will not attach to or close it.'
        }
        $currentRun = [ordered]@{round=$round;directory=(Join-Path $run "launch-$round")}
        $result.runs += $currentRun
        # Preserve old evidence but make it impossible to read a previous
        # process's active snapshot. The persistent sidecar stays in place.
        if ($round -gt 0) {
            foreach ($path in @($statusPath,$observationPath)) {
                if (Test-Path -LiteralPath $path) { Move-Item -LiteralPath $path -Destination $result.runs[$round-1].directory }
            }
        }
        $environment = @{GPVST3_DATA_DIR=$dataDirectory;GPVST3_VST3_ROOT=$FixturePath;
            GPVST3_ENABLE_P2_EFFECT='0';GPVST3_ENABLE_P4_INPUT='0'}
        # Deliberately omit GPVST3_ENABLE_P2_HOOK: startup intent must install
        # its own backend through the ordinary product path.
        $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $currentRun.directory -McpRoot $McpRoot -Environment $environment
        $currentRun.process_start_filetime = [string]$process.StartTime.ToFileTimeUtc()
        $sessionPath = Join-Path $currentRun.directory 'mcp/native-session.json'
        $deadline = [DateTime]::UtcNow.AddSeconds(45)
        do {
            $process.Refresh()
            if ($process.HasExited) { throw 'Test host exited before publishing startup identity.' }
            $status = Read-StartupJson $statusPath
            $descriptor = Read-StartupJson $sessionPath
            if ($status.pid -eq $process.Id -and $descriptor.pid -eq $process.Id) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($status.pid -ne $process.Id -or $descriptor.pid -ne $process.Id) { throw 'Current-process startup/MCP identity was not published.' }
        $currentRun.identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
        $session = New-McpSession -SessionFile $sessionPath
        $currentRun.audio_before = (Invoke-McpTool $session gp_audio_device).configuration
        $currentRun.listener_before = Get-StartupListener
        if ($currentRun.audio_before.audioDevice -cne 'ASIO') { throw 'Current device is not ASIO; this test does not change the user device.' }
        $opened = Invoke-McpTool $session gp_open @{path=$scoreCopy}
        $document = (Wait-StartupOperation $opened.request 'opened').document
        Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
        $expectedMonitor = if ($round -eq 0) {0.35} else {0.55}
        $expectedFixture = if ($round -eq 0) {0.25} else {0.75}
        # This snapshot is taken before opening the input panel/editor or
        # writing any product control, proving automatic sidecar restoration.
        $currentRun.automatic_input = if ($round -eq 2) { Wait-StartupOff -FreshProcess } else { Wait-StartupActive $expectedMonitor }
        $panel = Get-StartupControl 'gpvst3InputEffectChainButton'
        Invoke-McpTool $session gp_trigger @{snapshot=$panel.snapshot;id=$panel.control.id} | Out-Null
        $monitorControl = Get-StartupControl 'gpvst3InputLowLatencyEnabled'
        $gainControl = Get-StartupControl 'gpvst3InputGain'
        $effectControl = Get-StartupControl ('gpvst3InputEnabled_' + $classId)
        $label = Get-StartupControl 'gpvst3InputMonitorStatus'
        $expectedChecked = $round -ne 2
        $expectedStatus = if ($expectedChecked) {'低延迟监听已生效'} else {'低延迟监听未启用'}
        if ($monitorControl.control.properties.checked -ne $expectedChecked -or -not $effectControl.control.properties.checked -or
            $gainControl.control.properties.value -ne $expectedMonitor -or
            -not ([string]$label.control.properties.text).StartsWith($expectedStatus)) { throw 'Restored input UI does not match saved settings.' }
        $currentRun.ui = @{monitor=$monitorControl.control.properties;gain=$gainControl.control.properties;
            effect=$effectControl.control.properties;status=$label.control.properties}
        $beforeEditor = [uint64](Read-StartupJson $observationPath).gp_hook.editor_request_generation
        $editorButton = Get-StartupControl ('gpvst3InputEditor_' + $classId)
        Invoke-McpTool $session gp_trigger @{snapshot=$editorButton.snapshot;id=$editorButton.control.id} | Out-Null
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $hook = (Read-StartupJson $observationPath).gp_hook
            if ([uint64]$hook.editor_request_generation -gt $beforeEditor -and $hook.editor_stage -eq 'visible' -and
                ([string]$hook.editor_identity).StartsWith("input`n") -and ([string]$hook.editor_identity).EndsWith($classId)) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ([uint64]$hook.editor_request_generation -le $beforeEditor -or $hook.editor_stage -ne 'visible' -or
            -not ([string]$hook.editor_identity).StartsWith("input`n") -or
            -not ([string]$hook.editor_identity).EndsWith($classId)) { throw 'The input fixture editor did not become current.' }
        $currentRun.fixture = Read-StartupFixture $expectedFixture -Dormant:($round -eq 2)
        $currentRun.processor_identity = "$($process.Id)/$($currentRun.process_start_filetime)/$($currentRun.fixture.instance)"
        if ($round -eq 0) {
            $fixtureGain = Get-StartupControl 'gpvst3TestGain'
            Invoke-McpTool $session gp_set_property @{snapshot=$fixtureGain.snapshot;id=$fixtureGain.control.id;property='value';value=0.75} | Out-Null
            $currentRun.fixture_after_edit = Read-StartupFixture 0.75
            $gainControl = Get-StartupControl 'gpvst3InputGain'
            Invoke-McpTool $session gp_set_property @{snapshot=$gainControl.snapshot;id=$gainControl.control.id;property='value';value=0.55} | Out-Null
            $currentRun.input_after_edit = Wait-StartupActive 0.55
        }
        if ($round -eq 2) {
            # Off may retain a prepared editor instance; it must never enter
            # the audio graph. Check both monitor and processor counters over
            # several UI timer updates instead of counting warm instances.
            Start-Sleep -Seconds 2
            $currentRun.fixture_final = Read-StartupFixture 0.75 -Dormant
            $currentRun.off_after_settle = Wait-StartupOff -FreshProcess
            if ($currentRun.fixture_final.instance -cne $currentRun.fixture.instance) { throw 'Saved Off unexpectedly replaced its dormant processor.' }
        } else {
            $currentRun.fixture_final = Read-StartupFixture 0.75 ([uint64]$currentRun.fixture.blocks)
        }
        $nativeEditor = Get-StartupControl 'gpvst3NativeEditorWindow'
        Invoke-McpTool $session gp_close_window @{snapshot=$nativeEditor.snapshot;id=$nativeEditor.control.id} | Out-Null
        $currentRun.input_final = if ($round -eq 2) { Wait-StartupOff -FreshProcess } else { Wait-StartupActive 0.55 }
        $savedMode = if ($round -eq 2) {'off'} else {'low_latency_overlay'}
        $currentRun.saved_before_exit = Assert-StartupSaved 0.55 0.75 $savedMode
        if ($round -eq 1) {
            # The only mode write in this test is an ordinary explicit Off
            # after the second successful automatic activation and state check.
            $monitorControl = Get-StartupControl 'gpvst3InputLowLatencyEnabled'
            Invoke-McpTool $session gp_set_property @{snapshot=$monitorControl.snapshot;id=$monitorControl.control.id;property='checked';value=$false} | Out-Null
            ++$result.input_switch_writes
            $currentRun.off_after_request = Wait-StartupOff
            $monitorControl = Get-StartupControl 'gpvst3InputLowLatencyEnabled'
            if ($monitorControl.control.properties.checked -ne $false) { throw 'The explicit Off request was not retained by the UI.' }
            $savedMode = 'off'
            $currentRun.saved_off_before_exit = Assert-StartupSaved 0.55 0.75 $savedMode
        }
        $currentRun.audio_after = (Invoke-McpTool $session gp_audio_device).configuration
        $currentRun.listener_after = Get-StartupListener
        if (($currentRun.audio_before | ConvertTo-Json -Compress -Depth 10) -cne ($currentRun.audio_after | ConvertTo-Json -Compress -Depth 10) -or
            $currentRun.listener_before -ne $currentRun.listener_after) { throw 'Native audio/listener configuration changed during the startup regression.' }
        if ($round -gt 0 -and (($currentRun.audio_before | ConvertTo-Json -Compress -Depth 10) -cne
                ($result.runs[0].audio_after | ConvertTo-Json -Compress -Depth 10) -or $currentRun.listener_before -ne $result.runs[0].listener_after)) {
            throw 'Native audio/listener configuration did not persist unchanged across restart.'
        }
        Close-StartupHost
        $currentRun.saved_after_exit = Assert-StartupSaved 0.55 0.75 $savedMode
        if ($result.cleanup_errors.Count) { throw 'Startup regression cleanup failed.' }
    }
    # Virtual addresses may be reused in a different process. The process
    # start identity plus fixture address is the meaningful instance identity.
    if ($result.runs[0].processor_identity -ceq $result.runs[1].processor_identity -or
        $result.runs[0].process_start_filetime -ceq $result.runs[1].process_start_filetime) { throw 'Restart did not prove a fresh processor lifetime.' }
    if (@($result.runs.process_start_filetime | Select-Object -Unique).Count -ne 3 -or $result.input_switch_writes -ne 1) {
        throw 'Expected three fresh process lifetimes and exactly one explicit Off request.'
    }
    $result.status = 'pass'
} catch {
    $result.status = 'failed'; $result.failure = $_.Exception.Message; $result.failure_stack = $_.ScriptStackTrace
} finally {
    Close-StartupHost
    try { Assert-Gpvst3HostUnchanged $before $HostDirectory $run } catch { $result.cleanup_errors += $_.Exception.Message }
    if ((Get-FileHash -LiteralPath $seedSource).Hash -cne $seedHash) { $result.cleanup_errors += 'The source score changed.' }
    if ($result.cleanup_errors.Count) { $result.status = 'failed' }
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
}
if ($result.status -ne 'pass') { throw "P13 input startup regression failed: $($result.failure). Evidence: $run" }
Write-Output "PASS: production input restored on two launches and stayed Off on the third; UI/fixture gain persistence, ASIO 192000/64 and clean exit. Evidence: $run"
