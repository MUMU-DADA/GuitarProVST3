param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = '',
    [string]$ScorePath = '',
    [string]$OutputRoot = '',
    [ValidateRange(3, 120)][int]$CaptureSeconds = 15,
    [ValidateRange(0, 60000)][int]$ProbeDelayMilliseconds = 8000,
    [switch]$SilenceInputExperiment,
    [switch]$InspectAudioUnits,
    [switch]$ProbeListener,
    [switch]$EnableNativeListener,
    [switch]$SinkListenerExperiment,
    [switch]$PcmProbe,
    [switch]$StreamLifecycleProbe,
    [switch]$ProductionRuntime,
    [switch]$RseVst3,
    [switch]$RestartAsioStream,
    [switch]$DrainProbe,
    [switch]$OverlayCoexistenceProbe,
    [switch]$OverlayTransitionProbe,
    [switch]$NativeTailProbe,
    [switch]$DeviceMatrix,
    [switch]$ScopeMatrix,
    [switch]$NativeEffectsMatrix,
    [switch]$NativeListenerSwitches,
    [string]$InputOverlayFixture = '',
    [ValidateRange(0, 20)][int]$InputOverlaySwitches = 0,
    [ValidateRange(0, 20)][int]$RapidInputSwitches = 0,
    [switch]$LoopbackInput2,
    [ValidateSet('', 'native', 'overlay')][string]$MonitorLatency = ''
)

# Collects evidence only. A trace or a successful collection is not a P13 PASS.
# The silence experiment additionally requires a DLL built with the test-only
# experiment define. The production DLL must reject this environment request.
$ErrorActionPreference = 'Stop'
if ($ProductionRuntime -and (-not $InputOverlayFixture -or $PcmProbe -or $StreamLifecycleProbe -or $RestartAsioStream -or $DrainProbe -or $OverlayCoexistenceProbe -or $ProbeListener -or $SilenceInputExperiment -or $SinkListenerExperiment -or $MonitorLatency -or $InspectAudioUnits -or $LoopbackInput2)) {
    throw '-ProductionRuntime requires an input fixture and excludes experimental probes.'
}
if ($RseVst3 -and (-not $InputOverlayFixture -or $MonitorLatency)) { throw '-RseVst3 requires a fixture and excludes latency-only measurement.' }
if ($DeviceMatrix -and (-not $InputOverlayFixture -or $MonitorLatency -or $LoopbackInput2)) { throw '-DeviceMatrix requires input overlay without loopback remapping.' }
if ($ScopeMatrix -and (-not $RseVst3 -or $OverlayCoexistenceProbe)) { throw '-ScopeMatrix requires RseVst3 and excludes the unity-gain sample probe.' }
if ($OverlayTransitionProbe -and (-not $OverlayCoexistenceProbe -or $InputOverlaySwitches -or $RapidInputSwitches -or $ScopeMatrix -or $DeviceMatrix)) { throw '-OverlayTransitionProbe requires coexistence observation and excludes other switching matrices.' }
if (($InputOverlaySwitches -or $RapidInputSwitches) -and -not $InputOverlayFixture) { throw 'Input switches require an input overlay fixture.' }
if ($NativeTailProbe -and (-not $OverlayTransitionProbe -or -not $NativeEffectsMatrix)) { throw '-NativeTailProbe requires native effects and the transition probe.' }
if ($NativeEffectsMatrix -and (-not $EnableNativeListener -or -not $RseVst3 -or $MonitorLatency -or $LoopbackInput2)) { throw '-NativeEffectsMatrix requires native listener and RseVst3 without loopback channel changes.' }
if ($NativeListenerSwitches -and (-not $ProductionRuntime -or -not $EnableNativeListener -or -not $RseVst3)) { throw '-NativeListenerSwitches requires production runtime, native listener and RseVst3.' }
if ($OverlayCoexistenceProbe -and (-not $RseVst3 -or -not $StreamLifecycleProbe -or -not $EnableNativeListener -or $DrainProbe -or $ProbeListener -or $MonitorLatency)) {
    throw '-OverlayCoexistenceProbe requires RseVst3, lifecycle and native listener, excluding other listener/latency probes.'
}

if ($MonitorLatency -and (-not $PcmProbe -or -not $StreamLifecycleProbe -or -not $EnableNativeListener -or $LoopbackInput2 -or $DrainProbe -or $SinkListenerExperiment -or $SilenceInputExperiment)) {
    throw '-MonitorLatency requires PCM, lifecycle and native listener; it excludes other signal-routing experiments.'
}
if (($MonitorLatency -eq 'overlay' -and -not $InputOverlayFixture) -or ($MonitorLatency -eq 'native' -and $InputOverlayFixture)) {
    throw '-MonitorLatency overlay requires a fixture; native baseline forbids an overlay fixture.'
}
if ($InputOverlayFixture -and ($DrainProbe -or $SinkListenerExperiment -or $SilenceInputExperiment -or $LoopbackInput2)) {
    throw '-InputOverlayFixture cannot combine with other routing experiments.'
}
if ($InputOverlayFixture) { $InputOverlayFixture = (Resolve-Path -LiteralPath $InputOverlayFixture).Path }
if ($SinkListenerExperiment -and (-not $ProbeListener -or -not $EnableNativeListener)) {
    throw '-SinkListenerExperiment requires both -ProbeListener and -EnableNativeListener.'
}
if ($PcmProbe -and ($SilenceInputExperiment -or $SinkListenerExperiment)) { throw '-PcmProbe cannot be combined with input silence or listener sink experiments.' }
if ($RestartAsioStream -and -not $StreamLifecycleProbe) { throw '-RestartAsioStream requires -StreamLifecycleProbe.' }
if ($DrainProbe -and (-not $SinkListenerExperiment -or -not $StreamLifecycleProbe -or $SilenceInputExperiment)) {
    throw '-DrainProbe requires -SinkListenerExperiment -StreamLifecycleProbe and forbids -SilenceInputExperiment.'
}
if ($LoopbackInput2 -and (-not $PcmProbe -or $EnableNativeListener)) { throw '-LoopbackInput2 requires -PcmProbe and cannot retain native monitoring through -EnableNativeListener.' }
if ($PcmProbe -and -not $PSBoundParameters.ContainsKey('ProbeDelayMilliseconds')) { $ProbeDelayMilliseconds=60000 }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p13-probe/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root 'artifacts' }
$hostExe = Join-Path $HostDirectory 'GuitarPro.exe'
$mcpPlugin = Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll'
$mcpClient = Join-Path $McpRoot 'native/mcp-client.ps1'
foreach ($path in @($hostExe, $PluginPath, $mcpPlugin, $mcpClient)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "P13 collection prerequisite not found: $path" }
}
$probeBuildMarker = 'GPVST3_P13_EXPERIMENTAL_DIAGNOSTICS_NOT_FOR_RELEASE'
if (-not $ProductionRuntime -and -not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $PluginPath).Path)).Contains($probeBuildMarker)) {
    throw 'P13 collection requires a dedicated test DLL built by native/build.ps1 -EnableP13Probe. Production DLLs do not enable this probe or the silence experiment.'
}
if ($ProductionRuntime -and [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $PluginPath).Path)).Contains($probeBuildMarker)) { throw 'Production runtime validation rejects experimental DLLs.' }
if (@(Get-Process -Name GuitarPro -ErrorAction SilentlyContinue).Count) {
    throw 'An existing Guitar Pro process is running. This collector does not attach to or close user processes.'
}
if ((Get-Item -LiteralPath $hostExe).VersionInfo.FileVersion -ne '8.1.1.17') {
    throw 'P13 collection requires Guitar Pro 8.1.1.17.'
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'native/host_manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$moduleIdentity = foreach ($entry in $manifest.files.PSObject.Properties) {
    $path = Join-Path $HostDirectory $entry.Name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($hash -ine [string]$entry.Value) { throw "P13 host module hash mismatch: $($entry.Name)" }
    [ordered]@{name=$entry.Name;sha256=$hash}
}
$seedSource = Join-Path $McpRoot 'native/testdata/minimal.gp'
if (-not (Test-Path -LiteralPath $seedSource)) { $seedSource = Join-Path $McpRoot 'test/testdata/minimal.gp' }
if ($ScorePath) { $scoreSource = (Resolve-Path -LiteralPath $ScorePath).Path }
else {
    if (-not (Test-Path -LiteralPath $seedSource -PathType Leaf)) { throw "Minimal score fixture not found under $McpRoot." }
    $scoreSource = (Resolve-Path -LiteralPath $seedSource).Path
}
$sourceHash = (Get-FileHash -LiteralPath $scoreSource -Algorithm SHA256).Hash
$run = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('p13-host-' + [guid]::NewGuid().ToString('N'))
$dataDirectory = Join-Path $run 'data'
$emptyVst3Root = Join-Path $run 'empty-vst3'
New-Item -ItemType Directory -Path $dataDirectory, $emptyVst3Root -Force | Out-Null
$scoreCopy = Join-Path $run ('p13-score' + [IO.Path]::GetExtension($scoreSource))
Copy-Item -LiteralPath $scoreSource -Destination $scoreCopy
. (Join-Path $PSScriptRoot 'host-session.ps1')
. $mcpClient
if ($LoopbackInput2 -or $MonitorLatency -or $NativeEffectsMatrix) { . (Join-Path $PSScriptRoot 'p13-loopback-ui.ps1') }

function Set-P13NativeEffects([bool]$Enabled, [string]$Phase) {
    Open-P13LineInPopup
    $control=Get-P13ExactControl 'processDspChainSwitchBox' 'am::gui::SwitchBox'
    if ($null -eq $result.native_effects.original_checked) { $result.native_effects.original_checked=[bool]$control.control.properties.checked }
    $evidence=[ordered]@{phase=$Phase;before=$control.control.properties.checked;desired=$Enabled}
    $result.native_effects.operations += $evidence
    if ($control.control.properties.checked -ne $Enabled) {
        if (-not $control.control.enabled -or 'checked' -notin $control.control.writable_properties) { throw 'Native input effects switch is not writable.' }
        $evidence.response=Invoke-McpTool $session gp_set_property @{snapshot=$control.snapshot;id=$control.control.id;property='checked';value=$Enabled}
    }
    $readback=Get-P13ExactControl 'processDspChainSwitchBox' 'am::gui::SwitchBox'
    $evidence.confirmed=$readback.control.properties.checked -eq $Enabled
    if (-not $evidence.confirmed) { throw 'Native input effects readback mismatch.' }
    Close-P13LineInPopup
    Restore-P13LoopbackCursor
}

function Read-P13NativeMeter([string]$Phase) {
    Open-P13LineInPopup
    $samples=@()
    for ($index=0; $index -lt 12; ++$index) {
        $meter=Get-P13ExactControl 'preGainVolumeSlider' 'am::gui::VolumeSlider'
        $values=$meter.control.properties
        foreach ($value in @($values.leftChannelValue,$values.rightChannelValue)) {
            if ($null -eq $value -or [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value)) { throw 'Native input UI meter is unavailable/nonfinite.' }
        }
        $samples += @{left=$values.leftChannelValue;right=$values.rightChannelValue;value=$values.value;visible=$meter.control.visible}
        Start-Sleep -Milliseconds 100
    }
    $result.native_effects.meters += @{phase=$Phase;samples=$samples}
    Close-P13LineInPopup
    Restore-P13LoopbackCursor
}

function Read-P13Json([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    # A control-thread writer can be publishing while the collector reads.
    for ($attempt = 0; $attempt -lt 5; ++$attempt) {
        try { return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json }
        catch { if ($attempt -eq 4) { throw }; Start-Sleep -Milliseconds 50 }
    }
}

function Wait-P13Operation([string]$Request, [string]$Expected) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $state = (Invoke-McpTool $session gp_operation @{request=$Request}).operation
        if ($state.status -eq $Expected) { return $state }
        if ($state.status -in @('error', 'cancelled')) { throw "P13 operation ended as $($state.status): $Request" }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "P13 operation did not reach $Expected within 20 seconds: $Request"
}

function Get-P13ClockNanoseconds {
    # MSVC steady_clock and .NET Stopwatch both use the Windows QPC epoch.
    return [uint64][decimal]::Truncate(([decimal][Diagnostics.Stopwatch]::GetTimestamp() * 1000000000) / [Diagnostics.Stopwatch]::Frequency)
}

function Enable-P13InputOverlay {
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        $catalog=(Read-P13Json $statusPath).vst3_catalog
        $entry=@($catalog | Where-Object { [IO.Path]::GetFullPath($_.module) -ieq $InputOverlayFixture -and $_.class_id -and $_.recognition_status -eq 'ready' })
        if ($entry.Count -ne 1) { Start-Sleep -Milliseconds 200; continue }
        $name='gpvst3InputEnabled_' + $entry[0].class_id
        $query=Invoke-McpTool $session gp_objects @{query=$name;limit=10}
        $targets=@($query.objects | Where-Object object_name -CEQ $name)
        if ($targets.Count -eq 1) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($targets.Count -ne 1) { throw 'P13 input gain fixture checkbox was not found.' }
    $result.input_overlay.selection=Invoke-McpTool $session gp_set_property @{snapshot=$query.snapshot;id=$targets[0].id;property='checked';value=$true}
    if ($MonitorLatency) {
        $query=Invoke-McpTool $session gp_objects @{query='gpvst3InputGain';limit=10}
        $gain=@($query.objects | Where-Object object_name -CEQ 'gpvst3InputGain')
        if ($gain.Count -ne 1) { throw 'Input monitor measurement gain control was not found.' }
        $result.monitor_latency.overlay_gain_request=Invoke-McpTool $session gp_set_property @{snapshot=$query.snapshot;id=$gain[0].id;property='value';value=2.0}
    }
    $query=Invoke-McpTool $session gp_objects @{query='gpvst3InputLowLatencyEnabled';limit=10}
    $targets=@($query.objects | Where-Object object_name -CEQ 'gpvst3InputLowLatencyEnabled')
    if ($targets.Count -ne 1) { throw 'P13 low-latency control was not found.' }
    $result.input_overlay.monitor=Invoke-McpTool $session gp_set_property @{snapshot=$query.snapshot;id=$targets[0].id;property='checked';value=$true}
    $result.input_overlay.transitions=@()
    Wait-P13InputStatus '低延迟监听已生效'
}

function Invoke-P13InputSwitches {
    for ($round=0; $round -lt $InputOverlaySwitches; ++$round) {
        foreach ($enabled in @($false,$true)) {
            $query=Invoke-McpTool $session gp_objects @{query='gpvst3InputLowLatencyEnabled';limit=10}
            $target=@($query.objects | Where-Object object_name -CEQ 'gpvst3InputLowLatencyEnabled')
            if ($target.Count -ne 1) { throw 'Input monitor control disappeared during switching.' }
            $response=Invoke-McpTool $session gp_set_property @{snapshot=$query.snapshot;id=$target[0].id;property='checked';value=$enabled}
            $result.input_overlay.transitions += @{round=$round;enabled=$enabled;response=$response}
            Wait-P13InputStatus $(if($enabled){'低延迟监听已生效'}else{'低延迟监听未启用'})
        }
    }
}

function Enable-P13RseVst3 {
    $catalog=(Read-P13Json $statusPath).vst3_catalog
    $entry=@($catalog | Where-Object { [IO.Path]::GetFullPath($_.module) -ieq $InputOverlayFixture -and $_.class_id -and $_.recognition_status -eq 'ready' })
    if ($entry.Count -ne 1) { throw 'RSE coexistence requires the single-class gain fixture.' }
    $result.rse_vst3=@()
    foreach ($prefix in @('gpvst3Enabled_', 'gpvst3GlobalEnabled_')) {
        $name=$prefix + $entry[0].class_id
        $query=Invoke-McpTool $session gp_objects @{query=$name;limit=10}
        $item=@($query.objects | Where-Object object_name -CEQ $name)
        if ($item.Count -ne 1) { throw "RSE checkbox missing: $name" }
        $response=Invoke-McpTool $session gp_set_property @{snapshot=$query.snapshot;id=$item[0].id;property='checked';value=$true}
        $deadline=[DateTime]::UtcNow.AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 100
            $query=Invoke-McpTool $session gp_objects @{query=$name;limit=10}
            $item=@($query.objects | Where-Object object_name -CEQ $name)
        } while (($item.Count -ne 1 -or -not $item[0].properties.checked) -and [DateTime]::UtcNow -lt $deadline)
        if ($item.Count -ne 1 -or -not $item[0].properties.checked) { throw "RSE selection not retained: $name" }
        $result.rse_vst3 += @{control=$name;response=$response;readback=$item[0].properties.checked}
    }
    $result.native_track_before=Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='state'}
}

function Invoke-P13NativeListenerSwitches {
    $result.input_overlay.native_switches=@()
    foreach ($nativeEnabled in @($false,$true)) {
        Set-P13NativeListener $nativeEnabled 'overlay_native_user_change'
        Wait-P13InputStatus '低延迟监听已生效'
        foreach ($overlayEnabled in @($false,$true)) {
            Set-P13Check 'gpvst3InputLowLatencyEnabled' $overlayEnabled
            Wait-P13InputStatus $(if($overlayEnabled){'低延迟监听已生效'}else{'低延迟监听未启用'})
            $current=Get-P13NativeListenerAction
            $deadline=[DateTime]::UtcNow.AddSeconds(5)
            do {
                $monitor=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook.input_monitor
                if ($monitor.state -eq $(if($overlayEnabled){'active'}else{'off'}) -and
                    [bool]$monitor.input_native_effect_bypass -eq $overlayEnabled) { break }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $deadline)
            $result.input_overlay.native_switches += @{native_enabled=$nativeEnabled;overlay_enabled=$overlayEnabled;native_readback=$current.action.checked;monitor=$monitor}
            if ($current.action.checked -ne $nativeEnabled -or
                $monitor.state -ne $(if($overlayEnabled){'active'}else{'off'}) -or
                [bool]$monitor.input_native_effect_bypass -ne $overlayEnabled -or $monitor.error_blocks -ne '0') {
                throw 'Overlay switching changed the current native-listener choice or failed to settle.'
            }
        }
    }
}

function Invoke-P13RapidInputSwitches {
    $result.input_overlay.rapid_transitions=@()
    Set-P13Check 'gpvst3InputLowLatencyEnabled' $false
    Wait-P13InputStatus '低延迟监听未启用'
    for ($round=0; $round -lt $RapidInputSwitches; ++$round) {
        $q=Invoke-McpTool $session gp_objects @{query='gpvst3InputLowLatencyEnabled';limit=10}
        $item=@($q.objects | Where-Object object_name -CEQ 'gpvst3InputLowLatencyEnabled')
        if ($item.Count -ne 1) { throw 'Rapid switch control missing.' }
        Invoke-McpTool $session gp_set_property @{snapshot=$q.snapshot;id=$item[0].id;property='checked';value=$true} | Out-Null
        $during=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook.input_monitor
        # Query a fresh snapshot, then cancel without waiting for active.
        $q=Invoke-McpTool $session gp_objects @{query='gpvst3InputLowLatencyEnabled';limit=10}
        $item=@($q.objects | Where-Object object_name -CEQ 'gpvst3InputLowLatencyEnabled')
        if ($item.Count -ne 1) { throw 'Rapid switch control disappeared.' }
        Invoke-McpTool $session gp_set_property @{snapshot=$q.snapshot;id=$item[0].id;property='checked';value=$false} | Out-Null
        Wait-P13InputStatus '低延迟监听未启用'
        $deadline=[DateTime]::UtcNow.AddSeconds(5)
        do {
            $after=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook.input_monitor
            if ($after.state -eq 'off' -and -not $after.input_native_effect_bypass) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        $result.input_overlay.rapid_transitions += @{round=$round;during=$during;after=$after}
        if ($after.state -ne 'off' -or $after.input_native_effect_bypass) { throw 'Rapid cancellation retained suppression or an active input chain.' }
    }
    Set-P13Check 'gpvst3InputLowLatencyEnabled' $true
    Wait-P13InputStatus '低延迟监听已生效'
}

function Invoke-P13DeviceMatrix {
    $original = (Invoke-McpTool $session gp_audio_device).configuration
    if ($original.audioDevice -cne 'ASIO') { throw 'Device matrix requires an ASIO baseline.' }
    $result.device_matrix=[ordered]@{cases=@();restore_confirmed=$false}
    try {
        foreach ($case in @(
            @{property='audioBuffersSize';value=128;frames=128;state='active'},
            @{property='audioBuffersSize';value=256;frames=256;state='active'},
            @{property='audioBuffersSize';value=512;frames=512;state='active'},
            @{property='audioBuffersSize';value=1024;frames=1024;state='active'},
            @{property='audioBuffersSize';value=2048;frames=2048;state='active'},
            @{property='audioBuffersSize';value=4096;frames=0;state='host_limited'},
            @{property='audioBuffersSize';value=64;frames=64;state='active'},
            @{property='audioDevice';value='Standard';frames=0;state='host_limited'},
            @{property='audioDevice';value='ASIO';frames=64;state='active'})) {
            $before = Invoke-McpTool $session gp_audio_device
            if ($case.value -notin $before.choices.($case.property)) { throw "Device choice unavailable: $($case.property)=$($case.value)" }
            $entry=[ordered]@{property=$case.property;value=$case.value;expected_state=$case.state;before=$before;confirmed=$false}
            $result.device_matrix.cases += $entry
            try {
                $entry.request=Invoke-McpTool $session gp_audio_device @{operation='set';property=$case.property;value=$case.value}
            } catch {
                if ($_.Exception.Message -notlike '*Native device change failed; previous setting restored*') { throw }
                $entry.rejection=$_.Exception.Message
                $entry.device=Invoke-McpTool $session gp_audio_device
                if (-not $entry.device.running -or ($entry.device.configuration | ConvertTo-Json -Compress) -cne
                    ($before.configuration | ConvertTo-Json -Compress)) { throw 'Rejected device change did not retain the prior running configuration.' }
                Wait-P13InputStatus '低延迟监听已生效'
                $entry.confirmed=$true
                $entry.status='host_rejected_restored'
                continue
            }
            $deadline=[DateTime]::UtcNow.AddSeconds(12)
            do {
                Start-Sleep -Milliseconds 200
                $device=Invoke-McpTool $session gp_audio_device
                $monitor=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook.input_monitor
                $entry.device=$device; $entry.monitor=$monitor
                if ($device.configuration.($case.property) -eq $case.value -and $device.running -eq $true -and
                    $monitor.state -eq $case.state -and $monitor.mode -eq 'low_latency_overlay' -and
                    (($case.state -eq 'host_limited' -and -not $monitor.input_native_effect_bypass) -or
                     ($case.state -eq 'active' -and $monitor.buffer_frames -eq $case.frames -and
                      $monitor.process_frames -eq $case.frames -and $monitor.driver_buffer_frames -eq $case.frames))) {
                    $entry.confirmed=$true; break
                }
            } while ([DateTime]::UtcNow -lt $deadline)
            if (-not $entry.confirmed) { throw "Device matrix failed: $($case.property)=$($case.value), expected $($case.state)." }
        }
    } finally {
        foreach ($property in @('audioDevice','audioOutput','audioInput','audioOutputChannels','audioBuffersSize')) {
            $current=Invoke-McpTool $session gp_audio_device
            if ($current.configuration.$property -ne $original.$property) {
                Invoke-McpTool $session gp_audio_device @{operation='set';property=$property;value=$original.$property} | Out-Null
            }
        }
        $restored=Invoke-McpTool $session gp_audio_device
        $result.device_matrix.restore_confirmed=($restored.configuration | ConvertTo-Json -Compress) -ceq ($original | ConvertTo-Json -Compress)
        if (-not $result.device_matrix.restore_confirmed) { throw 'Device matrix did not restore the original audio configuration.' }
    }
    Wait-P13InputStatus '低延迟监听已生效'
}

function Set-P13Check([string]$Name, [bool]$Enabled) {
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do {
        $q=Invoke-McpTool $session gp_objects @{query=$Name;limit=10}
        $item=@($q.objects | Where-Object object_name -CEQ $Name)
        if ($item.Count -eq 1) {
            if ($item[0].properties.checked -eq $Enabled) { return }
            try { Invoke-McpTool $session gp_set_property @{snapshot=$q.snapshot;id=$item[0].id;property='checked';value=$Enabled} | Out-Null }
            catch { if ($_.Exception.Message -notlike '*Observed object no longer exists*') { throw } }
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Checkbox did not retain requested state: $Name"
}

function Read-P13Fixture([string]$Scope, [double]$SetGain = -1) {
    $prefix=@{input='gpvst3InputEditor_';track='gpvst3Editor_';global='gpvst3GlobalEditor_'}[$Scope]
    $name=$prefix+'40302010605080701122334455667788'
    $beforeGeneration=[int64](Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook.editor_request_generation
    $opened=$false
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do {
        $q=Invoke-McpTool $session gp_objects @{query=$name;limit=10}
        $item=@($q.objects | Where-Object object_name -CEQ $name)
        if ($item.Count -eq 1) {
            try { Invoke-McpTool $session gp_trigger @{snapshot=$q.snapshot;id=$item[0].id} | Out-Null }
            catch { if ($_.Exception.Message -notlike '*Observed object no longer exists*') { throw } }
            Start-Sleep -Milliseconds 250
            $hook=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook
            if ([int64]$hook.editor_request_generation -gt $beforeGeneration -and $hook.editor_stage -eq 'visible' -and
                (($Scope -eq 'input') -eq ([string]$hook.editor_identity).StartsWith("input`n"))) { $opened=$true; break }
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $opened) { throw "The $Scope fixture editor did not become current." }
    Start-Sleep -Milliseconds 250
    if ($SetGain -ge 0) {
        $q=Invoke-McpTool $session gp_objects @{query='gpvst3TestGain';limit=10}
        $gain=@($q.objects | Where-Object object_name -CEQ 'gpvst3TestGain')
        if ($gain.Count -ne 1) { throw 'Fixture editor gain is ambiguous.' }
        Invoke-McpTool $session gp_set_property @{snapshot=$q.snapshot;id=$gain[0].id;property='value';value=$SetGain} | Out-Null
        Start-Sleep -Milliseconds 300
    }
    $q=Invoke-McpTool $session gp_objects @{query='gpvst3TestProcessor';limit=10}
    $states=@($q.objects | Where-Object { $_.object_name -ceq 'gpvst3TestProcessor' -and $_.properties.text })
    if ($states.Count -ne 1) { throw 'Fixture processor state is ambiguous.' }
    $state=$states[0].properties.text | ConvertFrom-Json
    if ($SetGain -ge 0 -and $state.gain -ne $SetGain) { throw "$Scope editor did not update its processor." }
    return $state
}

function Invoke-P13ScopeMatrix {
    $result.scope_matrix=[ordered]@{baseline=@{};after_controls=@{};after_document=@{};after_empty=@{}}
    foreach ($scope in @('input','track','global')) {
        $gain=@{input=0.25;track=0.5;global=0.75}[$scope]
        $state=Read-P13Fixture $scope $gain
        if ($state.blocks -le 0 -or $state.input_energy -le 0 -or
            [Math]::Abs($state.output_energy/$state.input_energy-$gain*$gain) -gt 0.00001) {
            throw "$Scope fixture did not process its independent gain."
        }
        $result.scope_matrix.baseline[$scope]=$state
    }
    $instances=@($result.scope_matrix.baseline.Values | ForEach-Object instance | Sort-Object -Unique)
    if ($instances.Count -ne 3) { throw 'Input, track and global share a processor.' }
    foreach ($prefix in @('gpvst3Enabled_','gpvst3GlobalEnabled_')) {
        Set-P13Check ($prefix+'40302010605080701122334455667788') $false
        Set-P13Check ($prefix+'40302010605080701122334455667788') $true
    }
    foreach ($scope in @('input','track','global')) {
        $state=Read-P13Fixture $scope
        $result.scope_matrix.after_controls[$scope]=$state
        $before=$result.scope_matrix.baseline[$scope]
        if ($state.instance -ne $before.instance -or $state.gain -ne $before.gain) { throw "$Scope instance/state changed during global/track switching." }
    }
    $savedDocument=$document
    $created=Invoke-McpTool $session gp_new @{template='Steel Guitar'}
    $other=(Wait-P13Operation $created.request 'created').document
    $script:ownedDocuments += $other
    $input=Read-P13Fixture 'input'
    $result.scope_matrix.after_document=$input
    if ($input.instance -ne $result.scope_matrix.baseline.input.instance -or $input.gain -ne 0.25) { throw 'Changing score changed input state.' }
    Invoke-McpTool $session gp_activate @{document=$savedDocument} | Out-Null
    foreach ($prefix in @('gpvst3Enabled_','gpvst3GlobalEnabled_')) {
        Set-P13Check ($prefix+'40302010605080701122334455667788') $true
    }
    Invoke-McpTool $session gp_playback @{operation='set_loop';document=$savedDocument;enabled=$true} | Out-Null
    Invoke-McpTool $session gp_playback @{operation='play';document=$savedDocument} | Out-Null
    Set-P13Check 'gpvst3InputEnabled_40302010605080701122334455667788' $false
    Wait-P13InputStatus '输入监听已静音'
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    do {
        $empty=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook.input_monitor
        if ($empty.state -eq 'muted' -and $empty.input_native_effect_bypass) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    $result.scope_matrix.empty=$empty
    if ($empty.state -ne 'muted' -or -not $empty.input_native_effect_bypass) { throw 'Empty input chain did not retain native suppression.' }
    Set-P13Check 'gpvst3InputEnabled_40302010605080701122334455667788' $true
    Wait-P13InputStatus '低延迟监听已生效'
    $input=Read-P13Fixture 'input'
    $result.scope_matrix.after_empty=$input
    if ($input.gain -ne 0.25) { throw 'Re-enabling input lost its parameter state.' }
    $result.scope_matrix.complete=$true
}

function Wait-P13InputStatus([string]$Expected) {
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do {
        $query=Invoke-McpTool $session gp_objects @{query='gpvst3InputMonitorStatus';limit=10}
        $target=@($query.objects | Where-Object object_name -CEQ 'gpvst3InputMonitorStatus')
        if ($target.Count -eq 1 -and ([string]$target[0].properties.text).StartsWith($Expected)) {
            $result.input_overlay.transitions += @{expected=$Expected;observed=$target[0].properties}
            return
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    $result.input_overlay.failed_status_query=$query
    throw "Input status did not reach '$Expected' through normal UI notification."
}

function Get-P13NativeListenerAction {
    $query=Invoke-McpTool $session gp_actions @{query='actionActivatedLineIn';limit=20}
    $actions=@($query.objects | Where-Object object_name -CEQ 'actionActivatedLineIn')
    if ($actions.Count -ne 1 -or $actions[0].checkable -ne $true -or $actions[0].checked -isnot [bool]) {
        $result.native_listener.invalid_action_query=$query
        throw 'Expected exactly one checkable actionActivatedLineIn with an observed checked state.'
    }
    return [pscustomobject]@{snapshot=$query.snapshot;action=$actions[0]}
}

function Set-P13NativeListener([bool]$Enabled, [string]$Phase) {
    $evidence=[ordered]@{phase=$Phase;desired_checked=$Enabled;trigger_attempted=$false;readback_confirmed=$false;observations=@()}
    $result.native_listener.operations += $evidence
    $dialogs=Invoke-McpTool $session gp_dialogs
    if ($dialogs.blocked) { $evidence.dialogs=$dialogs; throw "A modal dialog blocks native-listener $Phase; inspect collection.json." }
    $current=Get-P13NativeListenerAction
    $evidence.observations += [ordered]@{utc=[DateTime]::UtcNow.ToString('o');checked=$current.action.checked;enabled=$current.action.enabled}
    if ($current.action.checked -eq $Enabled) { $evidence.readback_confirmed=$true; return }
    if ($current.action.enabled -ne $true) {
        $evidence.window_restore=Invoke-McpTool $session gp_window @{state='restore'}
        $deadline=[DateTime]::UtcNow.AddSeconds(5)
        do {
            Start-Sleep -Milliseconds 100
            $dialogs=Invoke-McpTool $session gp_dialogs
            if ($dialogs.blocked) { $evidence.dialogs=$dialogs; throw "A modal dialog appeared before native-listener $Phase; inspect collection.json." }
            $current=Get-P13NativeListenerAction
            if ($current.action.enabled -eq $true) { break }
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($current.action.enabled -ne $true) { $evidence.final_action=$current.action; throw "actionActivatedLineIn remained disabled during $Phase." }
    }
    # Obtain the target from a fresh action snapshot immediately before the one
    # trigger. Never replay it after an unknown transport outcome.
    $current=Get-P13NativeListenerAction
    $readyDeadline=[DateTime]::UtcNow.AddSeconds(5)
    while ($current.action.checked -ne $Enabled -and $current.action.enabled -ne $true -and [DateTime]::UtcNow -lt $readyDeadline) {
        Start-Sleep -Milliseconds 100
        $current=Get-P13NativeListenerAction
    }
    if ($current.action.checked -ne $Enabled) {
        if ($current.action.enabled -ne $true) { throw 'Native-listener action became disabled before triggering.' }
        $evidence.trigger_attempted=$true
        $evidence.trigger_response=Invoke-McpTool $session gp_trigger @{snapshot=$current.snapshot;id=$current.action.id}
    }
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    do {
        $dialogs=Invoke-McpTool $session gp_dialogs
        if ($dialogs.blocked) { $evidence.dialogs=$dialogs; throw "Native-listener $Phase opened a modal dialog; no button was guessed. Inspect collection.json." }
        $current=Get-P13NativeListenerAction
        $evidence.observations += [ordered]@{utc=[DateTime]::UtcNow.ToString('o');checked=$current.action.checked;enabled=$current.action.enabled}
        if ($current.action.checked -eq $Enabled) { $evidence.readback_confirmed=$true; return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Native-listener $Phase did not reach checked=$Enabled within 5 seconds."
}

function Test-P13Complete($Probe, [bool]$RequireListener = $false) {
    $complete = $null -ne $Probe -and $Probe.enabled -eq $true -and $Probe.complete -eq $true -and
        [int]$Probe.capacity -gt 0 -and [int]$Probe.published -eq [int]$Probe.capacity -and
        @($Probe.records).Count -eq [int]$Probe.capacity
    if ($RequireListener) {
        $complete = $complete -and $Probe.listener_probe.requested -eq $true -and $Probe.listener_probe.installed -eq $true -and
            $Probe.listener_probe.complete -eq $true -and [int]$Probe.listener_probe.published -eq [int]$Probe.capacity -and
            @($Probe.listener_probe.records).Count -eq [int]$Probe.capacity
    }
    if ($PcmProbe) {
        $complete=$complete -and $Probe.pcm_probe.enabled -eq $true -and $Probe.pcm_probe.complete -eq $true -and
            [int]$Probe.pcm_probe.claimed -gt 0 -and $Probe.pcm_probe.claimed -eq $Probe.pcm_probe.published -and -not $Probe.pcm_probe.error
    }
    if ($DrainProbe) {
        $complete=$complete -and $Probe.drain_probe.requested -eq $true -and
            $Probe.drain_probe.complete -eq $true -and $Probe.drain_probe.published -eq 4096
    }
    return $complete
}

function Get-P13PcmValidation {
    $manifest=Read-P13Json (Join-Path $dataDirectory 'p13-pcm.json')
    $validation=[ordered]@{complete=$false;manifest='p13-pcm.json';errors=@();files=@();playback_bracketed=$false}
    if (-not $manifest -or $manifest.complete -ne $true -or $manifest.enabled -ne $true -or $manifest.error) {
        $validation.errors += 'PCM manifest is missing, disabled, incomplete, or reports an export error.'
        return $validation
    }
    $records=@($manifest.records)
    if ($manifest.schema -ne 1 -or $manifest.format -cne 'float32_little_endian_interleaved_per_record' -or
        $records.Count -eq 0 -or $records.Count -ne $manifest.claimed -or $records.Count -ne $manifest.published -or
        ([int64]$manifest.frames_reserved -ne 262144 -and $records.Count -ne 8192)) {
        $validation.errors += 'PCM schema, count, or bounded recording capacity is inconsistent.'
    }
    $captureBytes=[int64]0; $outputBytes=[int64]0; $frameOffset=[int64]0; $previousSequence=[uint64]0
    $firstNs=[uint64]::MaxValue; $lastNs=[uint64]0; $recordIndex=0
    $boundIdentity=$null
    foreach ($record in $records) {
        if ($LoopbackInput2 -and ($record.driver_channels_validated -ne $true -or
            $record.input_channels -ne 1 -or $record.output_channels -ne 2 -or
            @($record.driver_input_selectors).Count -ne 2 -or @($record.driver_output_selectors).Count -ne 2 -or
            $record.driver_input_selectors[0] -ne 1 -or $record.driver_input_selectors[1] -ne -1 -or
            $record.driver_output_selectors[0] -ne 0 -or $record.driver_output_selectors[1] -ne 1)) {
            $validation.errors += 'A PCM block did not validate Analog In 2 and Analog Out 1/2 driver selectors.'
            break
        }
        if ($StreamLifecycleProbe) {
            $identity=@($record.generation,$record.rate_revision,$record.actual_callback_rate) -join '/'
            if ($null -eq $boundIdentity) { $boundIdentity=$identity }
            if ($record.actual_rate_validated -ne $true -or [uint64]$record.generation -eq 0 -or
                [uint64]$record.rate_revision -eq 0 -or $record.actual_callback_rate -lt 8000 -or
                $record.actual_callback_rate -gt 768000 -or $identity -cne $boundIdentity) {
                $validation.errors += 'PCM rate was not validated for one unchanged ASIO callback generation/revision.'
                break
            }
        }
        $sequence=[uint64]$record.sequence; $timestamp=[uint64]$record.timestamp_ns
        if ($record.index -ne $recordIndex -or $record.frame_offset -ne $frameOffset -or
            $record.saved_frames -le 0 -or $record.saved_frames -gt $record.callback_frames -or $record.callback_frames -gt 2048 -or
            $record.capture_status -ne 3 -or $record.output_status -ne 3 -or $record.configuration_valid -ne $true -or
            $record.input_channels -notin @(1,2) -or $record.output_channels -notin @(1,2) -or
            $record.capture_byte_offset -ne $captureBytes -or $record.output_byte_offset -ne $outputBytes -or
            $record.capture_bytes -ne [int64]$record.saved_frames*$record.input_channels*4 -or
            $record.output_bytes -ne [int64]$record.saved_frames*$record.output_channels*4 -or
            $record.capture_nonfinite -ne 0 -or $record.output_nonfinite -ne 0 -or $record.changes -ne 0 -or
            [uint64]$record.status_flags -ne 0 -or $record.original_result -ne 0 -or
            ($previousSequence -ne 0 -and $sequence -ne $previousSequence+1) -or $timestamp -lt $lastNs) {
            $validation.errors += "Invalid or discontinuous PCM record at index $($record.index)."
            break
        }
        $frameOffset += [int64]$record.saved_frames; $captureBytes += [int64]$record.capture_bytes; $outputBytes += [int64]$record.output_bytes
        $previousSequence=$sequence; $lastNs=$timestamp
        ++$recordIndex
        if ($timestamp -lt $firstNs) { $firstNs=$timestamp }
    }
    if ($frameOffset -ne [int64]$manifest.frames_reserved -or [uint64]$manifest.busy_drops -ne 0 -or [uint64]$manifest.abandoned -ne 0) {
        $validation.errors += 'PCM frame total, writer drops, or abandoned tickets are inconsistent with a continuous recording.'
    }
    foreach ($side in @(@{key='capture';name='p13-pcm-capture.f32';bytes=$captureBytes},@{key='output';name='p13-pcm-output.f32';bytes=$outputBytes})) {
        $fileProperty=$side.key+'_file'; $hashProperty=$side.key+'_sha256'; $path=Join-Path $dataDirectory $side.name
        if ($manifest.$fileProperty -cne $side.name -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { $validation.errors += "Missing/unexpected PCM $($side.key) file."; continue }
        $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash; $length=(Get-Item -LiteralPath $path).Length
        $validation.files += @{name=$side.name;bytes=$length;sha256=$hash}
        if ($length -ne $side.bytes -or $hash -ine $manifest.$hashProperty) { $validation.errors += "PCM $($side.key) file length or SHA-256 mismatch." }
    }
    $validation.first_callback_ns=[string]$firstNs; $validation.last_callback_start_ns=[string]$lastNs
    $validation.playback_bracketed=$firstNs -ge [uint64]$result.playback_confirmed_ns -and $lastNs -le [uint64]$result.playback_last_confirmed_ns
    if (-not $validation.playback_bracketed) { $validation.errors += 'PCM callback starts were not bracketed by confirmed playback observations.' }
    if ($LoopbackInput2 -and $firstNs -lt [uint64]$result.loopback_input2.configured_ns) { $validation.errors += 'PCM started before input-2 configuration and native-monitor shutdown completed.' }
    $validation.records=$records.Count; $validation.frames=$frameOffset
    $validation.complete=$validation.errors.Count -eq 0
    $validation.note='Integrity and callback-start coverage only; this does not establish cable identity, correlation, calibrated rate, or physical latency.'
    return $validation
}

function Get-P13ListenerValidation($Probe) {
    $listener = $Probe.listener_probe
    $records = @($listener.records | Where-Object { $null -ne $_ })
    $parents = @{}
    foreach ($parent in $Probe.records) { $parents[[string]$parent.sequence] = $parent }
    if ($DrainProbe) {
        foreach ($parent in $Probe.drain_probe.records) { $parents[[string]$parent.sequence] = $parent }
    }
    $unknownEnabled=0; $missingParents=0; $outsideParents=0; $threadMismatches=0; $unstableState=0
    foreach ($record in $records) {
        if ($record.unit_enabled_at_entry -isnot [bool]) { ++$unknownEnabled }
        if ($record.state_pointer_stable -ne $true) { ++$unstableState }
        $parentSequence = [uint64]0
        if (-not [uint64]::TryParse([string]$record.callback_sequence, [ref]$parentSequence) -or $parentSequence -eq 0 -or
            -not $parents.ContainsKey([string]$parentSequence)) { ++$missingParents; continue }
        $parent = $parents[[string]$parentSequence]
        if ($record.thread -ne $parent.thread) { ++$threadMismatches }
        $start=[uint64]$record.timestamp_ns; $end=$start+[uint64]$record.observed_ns
        if ($start -lt [uint64]$parent.timestamp_ns -or $end -gt ([uint64]$parent.timestamp_ns+[uint64]$parent.callback_ns)) { ++$outsideParents }
    }
    $enabled=@($records | Where-Object unit_enabled_at_entry -EQ $true).Count
    $disabled=@($records | Where-Object unit_enabled_at_entry -EQ $false).Count
    $duplicates=$records.Count-@($records.sequence | Sort-Object -Unique).Count
    $valid=(Test-P13Complete $Probe $true) -and -not ($unknownEnabled+$missingParents+$outsideParents+$threadMismatches+$duplicates+$unstableState)
    return [ordered]@{
        status=$(if (-not $valid) { 'listener_evidence_incomplete_or_inconsistent' } elseif ($enabled -eq 0) { 'collected_inactive_listener' } else { 'collected_unvalidated' })
        complete_and_correlated=$valid
        requested=$listener.requested; installed=$listener.installed; complete=$listener.complete
        records=$records.Count; enabled_records=$enabled; disabled_records=$disabled; unknown_enabled_records=$unknownEnabled
        missing_parent_records=$missingParents; outside_parent_time_records=$outsideParents; parent_thread_mismatches=$threadMismatches
        duplicate_listener_sequences=$duplicates; unstable_state_records=$unstableState
        active_listener_path_observed=($enabled -gt 0)
        note='Disabled listener records cannot validate active native-input processing or bypass. Correlation is diagnostic evidence, not P13 acceptance.'
    }
}

function Get-P13Observation([string]$Phase) {
    $process.Refresh()
    if ($process.HasExited) { throw "P13 test process exited during $Phase ($($process.ExitCode))." }
    $queryStart = Get-P13ClockNanoseconds
    $queryUtc = [DateTime]::UtcNow.ToString('o')
    $playback = Invoke-McpTool $session gp_playback @{operation='state';document=$document}
    $queryEnd = Get-P13ClockNanoseconds
    return [ordered]@{
        phase=$Phase
        utc=$queryUtc
        elapsed_ms=$clock.ElapsedMilliseconds
        playback_query_start_ns=[string]$queryStart
        playback_query_end_ns=[string]$queryEnd
        playback=$playback
        observation=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json'))
        probe_file_present=(Test-Path -LiteralPath (Join-Path $dataDirectory 'p13-input-probe.json') -PathType Leaf)
    }
}

$before = Get-Gpvst3HostSnapshot $HostDirectory
$result = [ordered]@{
    schema=1
    production_runtime=[bool]$ProductionRuntime
    rse_vst3_requested=[bool]$RseVst3
    status='collecting'
    p13_acceptance='not_evaluated'
    silence_input_requested=[bool]$SilenceInputExperiment
    audio_units_inspection_requested=[bool]$InspectAudioUnits
    listener_probe_requested=[bool]$ProbeListener
    listener_sink_experiment_requested=[bool]$SinkListenerExperiment
    pcm_probe_requested=[bool]$PcmProbe
    stream_lifecycle_probe_requested=[bool]$StreamLifecycleProbe
    drain_probe_requested=[bool]$DrainProbe
    asio_stream_restart_requested=[bool]$RestartAsioStream
    loopback_input2=[ordered]@{requested=[bool]$LoopbackInput2;configured=$false;original_input_index=$null;original_gain=$null;restore_confirmed=$null;operations=@()}
    monitor_latency=[ordered]@{requested=[bool]$MonitorLatency;mode=$MonitorLatency;scope='raw_input1_to_monitored_right_output_to_raw_input2; test_only_input1_duplication';configured=$false}
    native_listener=[ordered]@{requested=[bool]$EnableNativeListener;original_checked=$null;restore_confirmed=$null;operations=@();note='UI checked-state readback is distinct from the listener DSP probe.'}
    native_effects=[ordered]@{requested=[bool]$NativeEffectsMatrix;original_checked=$null;operations=@();meters=@();restore_confirmed=$null}
    probe_delay_ms=$ProbeDelayMilliseconds
    capture_seconds=$CaptureSeconds
    host_modules=@($moduleIdentity)
    score_source=$scoreSource
    score_source_sha256=$sourceHash
    score_copy=$scoreCopy
    limitations=@(
        $(if ($LoopbackInput2) { 'Input channel temporarily selects input 2 with native monitoring muted/off; original input, gain and listener state are restored.' } else { 'No device setting is changed; the current backend may not be ASIO.' }),
        'Driver buffer, callback frames, input energy and output energy require separate trace interpretation.',
        'Collection does not prove tail/queue isolation, RSE invariance, recording/meter semantics or physical loopback latency.'
    )
    observations=@()
    cleanup_errors=@()
    clock_basis='Windows QPC; MSVC steady_clock and .NET Stopwatch; nanoseconds'
}
$process = $null
$session = $null
$document = $null
$initialPlayback = $null
$ownedDocuments = @()
$sessionCloseError = $null
$sessionCloseTransportFailure = $false
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    $environment = @{
        GPVST3_DATA_DIR=$dataDirectory
        GPVST3_ENABLE_P2_HOOK='1'
        GPVST3_ENABLE_P2_EFFECT='0'
        GPVST3_ENABLE_P4_INPUT='0'
        GPVST3_P13_PROBE='1'
        GPVST3_P13_PCM_PROBE=$(if ($PcmProbe) { '1' } else { '0' })
        GPVST3_P13_MONITOR_LATENCY=$(if ($MonitorLatency) { '1' } else { '0' })
        GPVST3_P13_STREAM_PROBE=$(if ($StreamLifecycleProbe) { '1' } else { '0' })
        GPVST3_P13_RESTART_STREAM=$(if ($RestartAsioStream) { '1' } else { '0' })
        GPVST3_P13_DRAIN_PROBE=$(if ($DrainProbe) { '1' } else { '0' })
        GPVST3_P13_OVERLAY_PROBE=$(if ($OverlayCoexistenceProbe) { '1' } else { '0' })
        GPVST3_P13_OVERLAY_TRANSITION=$(if ($OverlayTransitionProbe) { '1' } else { '0' })
        GPVST3_P13_PROBE_DELAY_MS=[string]$ProbeDelayMilliseconds
        GPVST3_P13_SILENCE_INPUT=$(if ($SilenceInputExperiment) { '1' } else { '0' })
        GPVST3_P13_LISTENER_PROBE=$(if ($ProbeListener) { '1' } else { '0' })
        GPVST3_P13_LISTENER_SINK=$(if ($SinkListenerExperiment) { '1' } else { '0' })
        GPVST3_P13_COPY_SCORE='1'
        GPVST3_VST3_ROOT=$emptyVst3Root
    }
    if ($ProductionRuntime) {
        foreach ($key in @($environment.Keys)) { if ($key.StartsWith('GPVST3_P13_')) { $environment.Remove($key) } }
    }
    if ($InputOverlayFixture) {
        $environment.GPVST3_VST3_ROOT=$InputOverlayFixture
        $result.input_overlay=[ordered]@{fixture=$InputOverlayFixture;acceptance='not_evaluated'}
    }
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -McpRoot $McpRoot -Environment $environment
    $result.pid = $process.Id
    $result.process_start_filetime = [string]$process.StartTime.ToFileTimeUtc()
    $statusPath = Join-Path $dataDirectory 'status.json'
    $sessionPath = Join-Path $run 'mcp/native-session.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        if ($process.HasExited) { throw "Guitar Pro exited before P13/MCP startup ($($process.ExitCode))." }
    }
    if (-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) { throw 'P13/MCP startup files were not published.' }
    $result.identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
    $descriptor = Read-P13Json $sessionPath
    if ($descriptor.pid -ne $process.Id) { throw 'The MCP descriptor belongs to another process.' }
    $session = New-McpSession -SessionFile $sessionPath
    $result.audio_device_before = Invoke-McpTool $session gp_audio_device
    $opened = Invoke-McpTool $session gp_open @{path=$scoreCopy}
    $document = (Wait-P13Operation $opened.request 'opened').document
    $ownedDocuments += $document
    if (-not $ScorePath) {
        # The minimal fixture primes score loading. The installed Steel Guitar
        # template supplies a real native RSE sound; all edits stay in this run.
        $created = Invoke-McpTool $session gp_new @{template='Steel Guitar'}
        $document = (Wait-P13Operation $created.request 'created').document
        $ownedDocuments += $document
        if (-not $MonitorLatency) {
            $riff = Invoke-McpTool $session gp_insert_tab @{document=$document;track=0;string=0;bar=0;text='0-2-5-7';mode='replace';denominator=4}
            Wait-P13Operation $riff.request 'applied' | Out-Null
        } else {
            $result.monitor_latency.rse_score='empty_Steel_Guitar_template; no_generated_notes; latency_only'
        }
        $scoreCopy = Join-Path $run 'p13-rse-playback.gp'
        $saved = Invoke-McpTool $session gp_save_as @{document=$document;path=$scoreCopy}
        if ($saved.status -ne 'saved') { Wait-P13Operation $saved.request 'saved' | Out-Null }
        $result.score_copy = $scoreCopy
    }
    Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
    if ($EnableNativeListener -or $LoopbackInput2 -or $MonitorLatency) {
        $initialListener=Get-P13NativeListenerAction
        $result.native_listener.original_checked=[bool]$initialListener.action.checked
        if ($LoopbackInput2 -or $MonitorLatency) {
            if ($result.audio_device_before.configuration.audioDevice -cne 'ASIO' -or $result.audio_device_before.configuration.audioInput -cne 'Midiplus USB Audio' -or
                $result.audio_device_before.configuration.audioOutput -cne 'Midiplus USB Audio' -or $result.audio_device_before.configuration.audioOutputChannels -ne 0) {
                throw '-LoopbackInput2 requires the observed Midiplus USB Audio ASIO configuration.'
            }
            if ($MonitorLatency) { Set-P13MonitorMeasurement } else { Set-P13LoopbackInput2 }
            Restore-P13LoopbackCursor
        } else { Set-P13NativeListener $true 'enable' }
    }
    if ($NativeEffectsMatrix) { Set-P13NativeEffects $true 'enable'; Read-P13NativeMeter 'native_before_overlay' }
    if ($InputOverlayFixture -and -not $OverlayTransitionProbe) { Enable-P13InputOverlay }
    if ($RseVst3) { Enable-P13RseVst3 }
    $initialPlayback = Invoke-McpTool $session gp_playback @{operation='state';document=$document}
    $result.observations += Get-P13Observation 'before_playback'
    Invoke-McpTool $session gp_playback @{operation='set_loop';document=$document;enabled=$true} | Out-Null
    $result.play_request = Invoke-McpTool $session gp_playback @{operation='play';document=$document}
    $playDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $started = Get-P13Observation 'awaiting_playback'
        $result.observations += $started
        if ($started.playback.playing -eq $true -and $started.playback.counting_down -ne $true) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $playDeadline)
    if ($started.playback.playing -ne $true -or $started.playback.counting_down -eq $true) { throw 'RSE playback was not observed running within 10 seconds.' }
    $result.playback_confirmed_ns = $started.playback_query_end_ns
    $result.playback_confirmed_utc = $started.utc
    if ($OverlayTransitionProbe) {
        # Warm the native input effects, RSE and shared SRC/ring first. Start
        # overlay after the observation deadline so its very first suppressed
        # callback is recorded, not a later steady-state approximation.
        $warmDeadline=[DateTime]::UtcNow.AddMilliseconds($ProbeDelayMilliseconds + 3000)
        do { Start-Sleep -Milliseconds 500 } while ([DateTime]::UtcNow -lt $warmDeadline)
        $result.observations += Get-P13Observation 'native_warmed_before_transition'
        if ($NativeTailProbe) {
            Open-P13LineInPopup
            $gain=Get-P13ExactControl 'preGainVolumeSlider' 'am::gui::VolumeSlider'
            $result.native_effects.original_gain=$gain.control.properties.value
            Set-P13LoopbackProperty 'preGainVolumeSlider' 'value' 0 'native_tail_cut_source'
            Close-P13LineInPopup
            Restore-P13LoopbackCursor
        }
        Enable-P13InputOverlay
    }
    if ($InputOverlaySwitches) {
        Invoke-P13InputSwitches
        $afterSwitches = Get-P13Observation 'after_playback_switches'
        $result.observations += $afterSwitches
        if ($afterSwitches.playback.playing -ne $true) { throw 'Playback stopped during input-mode switching.' }
    }
    if ($RapidInputSwitches) { Invoke-P13RapidInputSwitches }
    if ($NativeListenerSwitches) { Invoke-P13NativeListenerSwitches }
    if ($NativeEffectsMatrix) { Read-P13NativeMeter 'overlay_during_playback' }
    if ($InspectAudioUnits) {
        try {
            $result.audio_units_inspection = & (Join-Path $PSScriptRoot 'inspect-p13-audio-units.ps1') -TargetProcessId $process.Id -RunDirectory $run -HostDirectory $HostDirectory -ExpectedProcessStartFileTime ([string]$process.StartTime.ToFileTimeUtc()) -ExpectedPluginPath $PluginPath -DumpFunctionBytes
        } catch {
            $result.audio_units_inspection = [ordered]@{status='inspection_failed';error=$_.Exception.Message}
        }
    }
    $result.capture_wait_started_utc = [DateTime]::UtcNow.ToString('o')
    $captureStartMs = $clock.ElapsedMilliseconds
    $minimumCaptureMs = [int64]$CaptureSeconds * 1000
    # Keep playback running through the delayed bounded probe, even when a
    # caller requests a short observation duration. Never mistake a first,
    # empty or partial control-thread snapshot for a completed recording.
    $maximumCaptureMs = if ($ProductionRuntime) { $minimumCaptureMs } else { [Math]::Max($minimumCaptureMs, [int64]$ProbeDelayMilliseconds + 30000) }
    do {
        Start-Sleep -Milliseconds 500
        $observed = Get-P13Observation 'during_playback'
        $result.observations += $observed
        if ($observed.playback.playing -ne $true -or $observed.playback.counting_down -eq $true) { throw 'RSE playback stopped during the probe window.' }
        $currentProbe = Read-P13Json (Join-Path $dataDirectory 'p13-input-probe.json')
        $elapsedCaptureMs = $clock.ElapsedMilliseconds - $captureStartMs
        if ((Test-P13Complete $currentProbe ([bool]$ProbeListener)) -and $elapsedCaptureMs -ge $minimumCaptureMs) { break }
    } while ($elapsedCaptureMs -lt $maximumCaptureMs)
    $result.capture_wait_finished_utc = [DateTime]::UtcNow.ToString('o')
    $observed = Get-P13Observation 'before_stop'
    $result.observations += $observed
    if ($observed.playback.playing -ne $true) { throw 'RSE playback was not running after the probe wait.' }
    if ($ScopeMatrix) {
        Invoke-P13ScopeMatrix
        # Returning to a score restarts its transport/count-in and track
        # binding asynchronously. Require new processing before judging it.
        $deadline=[DateTime]::UtcNow.AddSeconds(12)
        do {
            $observed=Get-P13Observation 'after_scope_matrix'
            $result.observations += $observed
            if ($observed.playback.playing -and -not $observed.playback.counting_down -and
                @($observed.observation.gp_hook.track_runtime_evidence | Where-Object { $_.configured_effects -gt 0 -and $_.processed -and $_.error_blocks -eq 0 }).Count -gt 0) { break }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $deadline)
    }
    if ($RseVst3) {
        $hook = $observed.observation.gp_hook
        $result.rse_processing = [ordered]@{
            track_blocks=$hook.track_chain_processed_blocks
            global_blocks=$hook.global_chain_process_blocks
            global_errors=$hook.chain_error_blocks
            tracks=$hook.track_runtime_evidence
            input=$hook.input_monitor
        }
        if ($hook.track_chain_processed_blocks -le 0 -or $hook.global_chain_process_blocks -le 0 -or
            $hook.chain_error_blocks -ne 0 -or $hook.input_monitor.state -ne 'active' -or
            [uint64]$hook.input_monitor.error_blocks -ne 0 -or
            @($hook.track_runtime_evidence | Where-Object { $_.configured_effects -gt 0 -and $_.processed -and $_.error_blocks -eq 0 }).Count -eq 0) {
            throw 'RSE coexistence is missing active fault-free input, track or global processing.'
        }
    }
    $result.playback_last_confirmed_ns = $observed.playback_query_start_ns
    $result.playback_stop_requested_ns = [string](Get-P13ClockNanoseconds)
    Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null
    $stopDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 100
        $stopped = Get-P13Observation 'after_playback'
        $result.observations += $stopped
        if ($stopped.playback.playing -eq $false) { break }
    } while ([DateTime]::UtcNow -lt $stopDeadline)
    if ($stopped.playback.playing -ne $false) { throw 'RSE playback stop was not observed within 5 seconds.' }
    if ($DeviceMatrix) { Invoke-P13DeviceMatrix }
    $result.audio_device_after = Invoke-McpTool $session gp_audio_device
    $result.audio_configuration_unchanged = (ConvertTo-Json $result.audio_device_before.configuration -Depth 20 -Compress) -ceq
        (ConvertTo-Json $result.audio_device_after.configuration -Depth 20 -Compress)
    if ($RseVst3) {
        $result.native_track_after=Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='state'}
        if (($result.native_track_before.sounds | ConvertTo-Json -Depth 30 -Compress) -cne ($result.native_track_after.sounds | ConvertTo-Json -Depth 30 -Compress)) { throw 'Native track effect state changed during coexistence.' }
    }
    $result.final_status = Read-P13Json $statusPath
    $result.probe = Read-P13Json (Join-Path $dataDirectory 'p13-input-probe.json')
    $result.probe_file_present = $null -ne $result.probe
    $result.probe_complete = Test-P13Complete $result.probe
    if ($DrainProbe -or $OverlayCoexistenceProbe) {
        $drain=$result.probe.drain_probe
        $drainStart=[uint64]::MaxValue; $drainEnd=[uint64]0
        foreach ($record in $drain.records) {
            $recordStart=[uint64]$record.timestamp_ns
            $recordEnd=$recordStart+[uint64]$record.callback_ns
            if ($recordStart -lt $drainStart) { $drainStart=$recordStart }
            if ($recordEnd -gt $drainEnd) { $drainEnd=$recordEnd }
        }
        $drainBracketed=$drainStart -ge [uint64]$result.playback_confirmed_ns -and
            $drainEnd -le [uint64]$result.playback_last_confirmed_ns
        $result.drain_validation=[ordered]@{
            complete=$drain.complete -eq $true -and $drain.published -eq 4096 -and
                $drain.valid_records -eq 4096 -and $drain.ready_records -gt 0 -and $drain.overlaps -eq 0 -and $drainBracketed
            valid_records=$drain.valid_records; ready_records=$drain.ready_records; overlaps=$drain.overlaps
            first_callback_ns=[string]$drainStart; last_callback_end_ns=[string]$drainEnd
            bracketed_by_playing_observations=$drainBracketed
            note='Joint SRC/ring count evidence only; not signal-isolation, RSE or full P13 acceptance.'
        }
        if ($OverlayCoexistenceProbe) {
            $records=@($drain.records)
            $ready=@($records | Where-Object { $_.valid -and $_.drain_state -eq 2 })
            $result.overlay_validation=[ordered]@{
                complete=$result.drain_validation.complete -and $drain.overlay_probe -eq $true -and
                    @($records | Where-Object { -not $_.overlay_suppressed -or (-not $OverlayTransitionProbe -and -not $_.overlay_committed) -or $_.overlay_mismatch_samples -ne 0 -or $_.overlay_compared_samples -ne $_.frames * 2 }).Count -eq 0 -and
                    ($records | Measure-Object rse_compared_samples -Sum).Sum -gt 0 -and
                    ($records | Measure-Object pre_overlay_energy -Sum).Sum -gt 0 -and
                    ($records | Measure-Object overlay_energy -Sum).Sum -gt 0 -and
                    ($records | Measure-Object native_energy -Sum).Sum -gt 0 -and
                    ($records | Measure-Object listener_peak_samples -Sum).Sum -gt 0 -and
                    $ready.Count -gt 0 -and ($ready | Measure-Object rse_energy -Sum).Sum -gt 0 -and
                    ($ready | Measure-Object overlay_energy -Sum).Sum -gt 0 -and
                    @($ready.monitor_token | Sort-Object -Unique).Count -eq 1 -and
                    @($ready.generation | Sort-Object -Unique).Count -eq 1
                compared_mix_samples=($records | Measure-Object overlay_compared_samples -Sum).Sum
                compared_rse_samples=($records | Measure-Object rse_compared_samples -Sum).Sum
                pre_overlay_energy=($records | Measure-Object pre_overlay_energy -Sum).Sum
                rse_unit_energy=($records | Measure-Object rse_energy -Sum).Sum
                input_energy=($records | Measure-Object overlay_energy -Sum).Sum
                native_sink_energy=($records | Measure-Object native_energy -Sum).Sum
                listener_peak_observations=($records | Measure-Object listener_peak_samples -Sum).Sum
                ready_records=$ready.Count
                ready_rse_compared_samples=($ready | Measure-Object rse_compared_samples -Sum).Sum
                ready_rse_unit_energy=($ready | Measure-Object rse_energy -Sum).Sum
                ready_input_energy=($ready | Measure-Object overlay_energy -Sum).Sum
                scope='Same-callback sample identity with unity input fixture, native listener sink and separate RSE units; not all P13 gates.'
            }
            if (-not $result.overlay_validation.complete) { throw 'Actual overlay sample coexistence evidence failed.' }
            if ($OverlayTransitionProbe) {
                $preparing=@($records | Where-Object production_drain_state -EQ 1)
                $active=@($records | Where-Object production_drain_state -EQ 2)
                $result.overlay_transition=[ordered]@{
                    preparing_records=$preparing.Count; active_records=$active.Count
                    first_committed_sequence=$active[0].sequence
                    first_sequence=$records[0].sequence
                    native_sink_energy_preparing=($preparing | Measure-Object native_energy -Sum).Sum
                    complete=$drain.transition_probe -and $preparing.Count -gt 0 -and $active.Count -gt 0 -and
                        ($preparing | Measure-Object native_energy -Sum).Sum -gt 0 -and
                        @($records | Where-Object { $_.production_drain_state -ne $_.drain_state -or $_.production_drain_phase -ne $_.drain_phase -or $_.overlay_committed -ne ($_.drain_state -eq 2) }).Count -eq 0
                    scope='First suppressed callback through actual production drain and overlay; unchanged GP output until Ready; independent retrospective tracker agrees.'
                }
                if (-not $result.overlay_transition.complete) { throw 'Production transition did not match the independent drain observation.' }
                if ($NativeTailProbe) {
                    $tail=@($records | Where-Object { $_.listener_calls -gt 0 -and $_.native_pre_gain -eq 0 -and $_.native_energy -gt 0 })
                    $result.native_tail=[ordered]@{
                        blocks_with_tail=$tail.Count
                        tail_energy=($tail | Measure-Object native_energy -Sum).Sum
                        all_listener_calls_at_zero_gain=@($records | Where-Object { $_.listener_calls -gt 0 -and $_.native_pre_gain -ne 0 }).Count -eq 0
                        scope='Native pre-effect gain is zero; nonzero listener sink is residual DSP output; RSE SRC and overlay remain sample-verified.'
                    }
                    if (-not $result.native_tail.all_listener_calls_at_zero_gain -or $tail.Count -eq 0) { throw 'Native tail evidence requires zero pre-effect gain with nonzero sink output.' }
                }
            }
        }
    }
    if ($PcmProbe) { $result.pcm_validation=Get-P13PcmValidation }
    if ($ProbeListener) { $result.listener_validation = Get-P13ListenerValidation $result.probe }
    if ($result.probe -and @($result.probe.records).Count) {
        $startNs = [uint64]::MaxValue
        $endNs = [uint64]0
        foreach ($record in $result.probe.records) {
            $recordStart = [uint64]$record.timestamp_ns
            $recordEnd = $recordStart + [uint64]$record.callback_ns
            if ($recordStart -lt $startNs) { $startNs = $recordStart }
            if ($recordEnd -gt $endNs) { $endNs = $recordEnd }
        }
        $result.probe_window = [ordered]@{
            first_callback_ns=[string]$startNs
            last_measured_callback_end_ns=[string]$endNs
            duration_ms=([decimal]($endNs - $startNs) / 1000000)
            bracketed_by_playing_observations=($startNs -ge [uint64]$result.playback_confirmed_ns -and $endNs -le [uint64]$result.playback_last_confirmed_ns)
            continuous_playback_proven=$false
            note='Polling brackets the recorded window; it does not prove continuous playback or audible RSE output.'
        }
    }
    $process.Refresh()
    $result.external_vst3_modules = @($process.Modules | Where-Object FileName -Like '*.vst3' | ForEach-Object FileName)
    if ($InputOverlayFixture) {
        $allowedPrefix=$InputOverlayFixture.TrimEnd('\') + '\'
        $unexpected=@($result.external_vst3_modules | Where-Object { -not $_.StartsWith($allowedPrefix,[StringComparison]::OrdinalIgnoreCase) })
        if ($unexpected.Count) { throw 'An unexpected VST3 module was loaded during the input-overlay collection.' }
        $runtime=(Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')).gp_hook.input_monitor
        if (-not $runtime) { $runtime=$result.probe.input_monitor }
        $result.input_overlay.runtime=$runtime
        if ($runtime.state -ne 'active' -or [uint64]$runtime.processed_blocks -eq 0 -or
            (-not $DeviceMatrix -and [uint64]$runtime.error_blocks -ne 0)) { throw 'Input overlay did not produce a fault-free active observation.' }
    } elseif ($result.external_vst3_modules.Count) { throw 'An external VST3 module was loaded during the host-boundary collection.' }
    if (-not $result.audio_configuration_unchanged) { throw 'The observed audio configuration changed during collection.' }
    $result.status = if ($ProductionRuntime) { 'collected_unvalidated' }
        elseif (-not $result.probe_file_present) { 'probe_missing' }
        elseif (-not $result.probe_complete) { 'probe_partial' }
        elseif ($DrainProbe -and -not $result.drain_validation.complete) { 'drain_evidence_incomplete' }
        elseif ($PcmProbe -and -not $result.pcm_validation.complete) { 'pcm_evidence_incomplete' }
        elseif (-not $result.probe_window.bracketed_by_playing_observations) { 'playback_coverage_unverified' }
        elseif ($ProbeListener -and -not $result.listener_validation.complete_and_correlated) { 'listener_evidence_incomplete' }
        elseif ($EnableNativeListener -and $ProbeListener -and $result.listener_validation.enabled_records -ne $result.listener_validation.records) { 'listener_activation_unverified' }
        elseif ($InspectAudioUnits -and $result.audio_units_inspection.status -ne 'collected_unvalidated') { 'audio_units_inspection_incomplete' }
        elseif ($ProbeListener -and -not $LoopbackInput2 -and -not $result.listener_validation.active_listener_path_observed) { 'collected_inactive_listener' }
        else { 'collected_unvalidated' }
} catch {
    $result.status = 'collection_failed'
    $result.failure = $_.Exception.Message
    $result.failure_stack = $_.ScriptStackTrace
    throw
} finally {
    if ($session) {
        if ($NativeEffectsMatrix -and $null -ne $result.native_effects.original_checked) {
            try {
                if ($null -ne $result.native_effects.original_gain) {
                    Open-P13LineInPopup
                    Set-P13LoopbackProperty 'preGainVolumeSlider' 'value' ([int]$result.native_effects.original_gain) 'native_tail_restore_gain'
                    Close-P13LineInPopup
                }
                Set-P13NativeEffects ([bool]$result.native_effects.original_checked) 'restore'
                $result.native_effects.restore_confirmed=$true
            } catch { $result.native_effects.restore_confirmed=$false; $result.cleanup_errors += $_.Exception.Message }
            finally { try { Restore-P13LoopbackCursor } catch { $result.cleanup_errors += $_.Exception.Message } }
        }
        if ($LoopbackInput2 -or $MonitorLatency) {
            try {
                if ($document) { Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null }
                Restore-P13LoopbackInput
            } catch { $result.loopback_input2.restore_confirmed=$false; $result.cleanup_errors += $_.Exception.Message }
            finally {
                try { Restore-P13LoopbackCursor } catch { $result.cleanup_errors += $_.Exception.Message }
            }
        }
        if ($EnableNativeListener -and $result.native_listener.original_checked -is [bool]) {
            try {
                Set-P13NativeListener ([bool]$result.native_listener.original_checked) 'restore'
                $restored=Get-P13NativeListenerAction
                $result.native_listener.restore_confirmed=$restored.action.checked -eq $result.native_listener.original_checked
                if (-not $result.native_listener.restore_confirmed) { throw 'Native-listener state readback did not match the original checked state.' }
            } catch {
                $result.native_listener.restore_confirmed=$false
                $result.native_listener.restore_error=$_.Exception.Message
                $result.cleanup_errors += $_.Exception.Message
            }
        }
        try {
            if ($document -and $initialPlayback) {
                Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null
                Invoke-McpTool $session gp_playback @{operation='set_loop';document=$document;enabled=[bool]$initialPlayback.loop} | Out-Null
            }
            # Only documents opened/created by this collector can be discarded.
            # Close both the playback score and the priming minimal document
            # explicitly so a modified test score cannot leave a hidden prompt.
            $result.documents_before_cleanup = Invoke-McpTool $session gp_documents
            for ($index = $ownedDocuments.Count - 1; $index -ge 0; --$index) {
                $closed = Invoke-McpTool $session gp_close @{document=$ownedDocuments[$index];unsaved='discard'}
                Wait-P13Operation $closed.request 'closed' | Out-Null
            }
            $result.documents_after_cleanup = Invoke-McpTool $session gp_documents
            Request-Gpvst3McpHostExit $session $process
        } catch { $result.cleanup_errors += $_.Exception.Message }
        try { Close-McpSession $session; $result.session_close_status='closed_or_already_unavailable' }
        catch {
            $sessionCloseError=$_.Exception.Message; $result.session_close_error=$sessionCloseError
            $result.session_close_exception_type=$_.Exception.GetType().FullName
            $exception=$_.Exception
            while ($exception) {
                if ($exception -is [System.Net.Http.HttpRequestException] -or
                    ($exception -is [System.Net.WebException] -and $exception.Status -ne [System.Net.WebExceptionStatus]::ProtocolError)) {
                    $sessionCloseTransportFailure=$true
                }
                $exception=$exception.InnerException
            }
        }
    }
    if ($process) {
        try {
            $result.shutdown_process_identity_verified=$process.Id -eq $result.pid -and
                [string]$process.StartTime.ToFileTimeUtc() -ceq $result.process_start_filetime
        } catch { $result.shutdown_process_identity_verified=$false }
        try { Stop-Gpvst3TestHost $process -RunDirectory $run } catch { $result.cleanup_errors += $_.Exception.Message }
    }
    $result.runtime_final = Read-P13Json (Join-Path $dataDirectory 'p13-runtime-final.json')
    $result.final_observation = Read-P13Json (Join-Path $dataDirectory 'p2-observation.json')
    $result.shutdown = Read-P13Json (Join-Path $run 'shutdown.json')
    if ($StreamLifecycleProbe) {
        $result.stream_lifecycle_final = Read-P13Json (Join-Path $dataDirectory 'p13-stream-lifecycle-final.json')
    }
    if ($result.shutdown -and ($result.shutdown.forced -or -not $result.shutdown.exited -or $result.shutdown.exit_code -ne 0)) {
        $result.cleanup_errors += "Test host shutdown was not clean: forced=$($result.shutdown.forced), exited=$($result.shutdown.exited), exit_code=$($result.shutdown.exit_code)."
    }
    if ($sessionCloseError) {
        if ($sessionCloseTransportFailure -and $result.shutdown_process_identity_verified -and $result.shutdown.pid -eq $result.pid -and
            $result.shutdown.exited -eq $true -and $result.shutdown.forced -eq $false -and $result.shutdown.exit_code -eq 0) {
            $result.session_close_status='unavailable_after_verified_clean_exit'
        } else { $result.session_close_status='failed'; $result.cleanup_errors += $sessionCloseError }
    }
    try { Assert-Gpvst3HostUnchanged $before $HostDirectory $run } catch { $result.cleanup_errors += $_.Exception.Message }
    try {
        $result.score_source_unchanged = (Get-FileHash -LiteralPath $scoreSource -Algorithm SHA256).Hash -ceq $sourceHash
        if (-not $result.score_source_unchanged) { $result.cleanup_errors += 'The source score changed during collection.' }
    } catch { $result.cleanup_errors += $_.Exception.Message }
    $result.collection_status = $result.status
    $result.cleanup_status = if ($result.cleanup_errors.Count) { 'failed' } else { 'clean' }
    if ($result.cleanup_errors.Count -and $result.status -ne 'collection_failed') { $result.status = 'cleanup_failed' }
    $result | ConvertTo-Json -Depth 60 | Set-Content -LiteralPath (Join-Path $run 'collection.json') -Encoding UTF8
    Write-Output "P13 evidence: $run; status=$($result.status); acceptance=not_evaluated"
}
if ($result.cleanup_errors.Count) { throw "P13 cleanup failed. See $run/collection.json" }
if (-not $ProductionRuntime -and -not $result.probe_file_present) { throw "P13 probe file was not published. See $run/collection.json" }
if (-not $ProductionRuntime -and -not $result.probe_complete) { throw "P13 probe remained partial at the bounded deadline. See $run/collection.json" }
if ($PcmProbe -and -not $result.pcm_validation.complete) { throw "P13 PCM evidence was incomplete or failed integrity/coverage checks. See $run/collection.json" }
if (-not $ProductionRuntime -and -not $result.probe_window.bracketed_by_playing_observations) { throw "P13 probe window was not bracketed by confirmed RSE playback. Increase -ProbeDelayMilliseconds and collect a fresh run. See $run/collection.json" }
if ($ProbeListener -and -not $result.listener_validation.complete_and_correlated) { throw "P13 requested listener evidence was incomplete or uncorrelated. See $run/collection.json" }
if ($EnableNativeListener -and $ProbeListener -and $result.listener_validation.enabled_records -ne $result.listener_validation.records) { throw "P13 native-listener action was enabled but the entire probe window was not observed active. See $run/collection.json" }
if ($InspectAudioUnits -and $result.audio_units_inspection.status -ne 'collected_unvalidated') { throw "P13 requested audio-unit inspection was incomplete. See $run/collection.json and the inspection result_path." }
