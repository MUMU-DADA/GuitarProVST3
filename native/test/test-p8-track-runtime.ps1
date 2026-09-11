param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = '',
    [string]$Vst3Root = 'ParametricOD.vst3;Gateway.vst3',
    [switch]$CheckGain,
    [switch]$CheckLifecycle,
    [string]$ExpectedBindingSource = '',
    [ValidateSet('enabled', 'default')]
    [string]$HookMode = 'default',
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'
if ($CheckLifecycle) { $CheckGain = $true }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/p8-track-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
$mcpPlugin = Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll'
$mcpClient = Join-Path $McpRoot 'native/mcp-client.ps1'
foreach ($path in @((Join-Path $HostDirectory 'GuitarPro.exe'), $PluginPath, $mcpPlugin, $mcpClient)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "P8 track runtime prerequisite not found: $path" }
}

$run = Join-Path $root ('artifacts/mcp-p8-track-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
. (Join-Path $PSScriptRoot 'host-session.ps1')
. $mcpClient
$before = Get-Gpvst3HostSnapshot $HostDirectory
$dataDirectory = $run
$process = $null; $session = $null; $document = $null; $initialPlayback = $null
$result = [ordered]@{ run = $run; vst3_root = $Vst3Root }

function Json($value) { ConvertTo-Json -InputObject $value -Depth 30 -Compress }
function Wait-Operation([string]$request, [string]$expected) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20); $state = $null
    do {
        $state = Invoke-McpTool $session gp_operation @{request=$request}
        if ($state.operation.status -in @($expected, 'error', 'cancelled')) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($state.operation.status -ne $expected) { throw "Operation did not reach ${expected}: $(Json $state)" }
    $state.operation
}
function Wait-Track([int]$index) {
    $cursor = Invoke-McpTool $session gp_cursor @{document=$document;axis='track';index=$index}
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 250
        $state = Invoke-McpTool $session gp_audio_abi @{document=$document;operation='state'}
        $selected = @($state.tracks | Where-Object track_index -eq $index)
        $context = Invoke-McpTool $session gp_objects @{query='gpvst3TrackContext';limit=10}
        $label = @($context.objects | Where-Object parent_name -eq 'gpvst3P7Panel')[0]
        if ($selected.Count -gt 0 -and $label -and $label.properties.text -and $label.properties.text.EndsWith("Track $index")) { return $state }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Track $index did not become observable: $(Json $state)"
}
function Get-Observation() {
    $path = Join-Path $dataDirectory 'p2-observation.json'
    if (-not (Test-Path -LiteralPath $path)) { throw 'P8 realtime observation was not written.' }
    Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}
function Read-Gain([int]$track) {
    if ($track -ge 0) { Wait-Track $track | Out-Null }
    $prefix = if ($track -ge 0) {'gpvst3Editor_'} else {'gpvst3GlobalEditor_'}
    $query = Invoke-McpTool $session gp_objects @{query=($prefix + $candidates[0].class_id);limit=10}
    Invoke-McpTool $session gp_trigger @{snapshot=$query.snapshot;id=@($query.objects)[0].id} | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 150
        $query = Invoke-McpTool $session gp_objects @{query='gpvst3TestProcessor';limit=10}
        $value = @($query.objects)[0].properties.text
        if ($value) { return $value | ConvertFrom-Json }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Track $track processor GUI did not publish state."
}
function Assert-NativeLayout() {
    Invoke-McpTool $session gp_window @{state='restore'} | Out-Null
    $layouts = @()
    foreach ($scope in @('track','global')) {
        $pageName = if ($scope -eq 'track') {'tabTrackButton'} else {'tabScoreButton'}
        $page = Invoke-McpTool $session gp_objects @{query=$pageName;limit=10}
        if (-not $page.objects[0].properties.checked) {
            Invoke-McpTool $session gp_trigger @{snapshot=$page.snapshot;id=$page.objects[0].id} | Out-Null
        }
        Start-Sleep -Milliseconds 350
        $prefix = if ($scope -eq 'track') {'gpvst3Track'} else {'gpvst3Global'}
        $panelName = if ($scope -eq 'track') {'gpvst3P7Panel'} else {'gpvst3GlobalPanel'}
        $anchorName = if ($scope -eq 'track') {'soundRack'} else {'soundMastering'}
        $objects = (Invoke-McpTool $session gp_objects @{query='gpvst3';limit=200}).objects
        $anchor = (Invoke-McpTool $session gp_objects @{query=$anchorName;limit=10}).objects | Where-Object object_name -eq $anchorName
        $section = @($objects | Where-Object object_name -eq ($prefix + 'Vst3Section'))[0]
        $panel = @($objects | Where-Object object_name -eq $panelName)[0]
        $list = @($objects | Where-Object object_name -eq ($prefix + 'ChainList'))[0]
        $divider = @($objects | Where-Object object_name -eq ($prefix + 'Vst3Divider'))[0]
        if (-not $section.visible -or -not $panel.visible -or -not $list.visible -or -not $divider.visible -or
            -not $anchor.visible -or $panel.parent_name -ne $section.object_name -or $list.parent_name -ne $panelName -or
            $section.parent_name -ne $anchor.parent_name -or $section.properties.y -lt ($anchor.properties.y + $anchor.properties.height)) {
            throw "Native $scope section content/anchor/visibility failed: $(Json @{section=$section;panel=$panel;list=$list;anchor=$anchor})"
        }
        $layouts += @{scope=$scope;anchor=$anchor;objects=$objects}
    }
    $result.native_layout = $layouts
}
function Set-TrackEffect([int]$index, $candidate) {
    Wait-Track $index | Out-Null
    $panelEntry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
    if (@($panelEntry.objects).Count -eq 0) { throw 'VST3 sound-section entry was not found.' }
    Invoke-McpTool $session gp_trigger @{snapshot=$panelEntry.snapshot;id=@($panelEntry.objects)[0].id} | Out-Null
    $panelQuery = Invoke-McpTool $session gp_objects @{query='gpvst3P7Panel';limit=10}
    $panel = @($panelQuery.objects | Where-Object object_name -eq 'gpvst3P7Panel')[0]
    if (-not $panel -or $panel.parent_name -ne 'gpvst3TrackVst3Section') { throw 'Track content is not mounted in its native section.' }
    Start-Sleep -Milliseconds 250
    $query = Invoke-McpTool $session gp_objects @{query=('gpvst3Enabled_' + $candidate.class_id);limit=20}
    $checkbox = @($query.objects | Where-Object { $_.object_name -eq ('gpvst3Enabled_' + $candidate.class_id) })[0]
    if (-not $checkbox) { throw "Track $index checkbox was not found for $($candidate.class_id)." }
    if (-not $checkbox.enabled) { throw "Track $index checkbox is disabled: $(Json $checkbox)" }
    if (-not $checkbox.checked) {
        Invoke-McpTool $session gp_set_property @{snapshot=$query.snapshot;id=$checkbox.id;property='checked';value=$true} | Out-Null
        Start-Sleep -Milliseconds 500
    }
    return Invoke-McpTool $session gp_objects @{query='gpvst3Status';limit=10}
}

function Assert-NativeEffects() {
    $before = Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='state'}
    $effect = @($before.sounds[0].effects)[0]
    if (-not $effect -or @($effect.parameters).Count -eq 0) { throw 'Native RSE effect/parameter baseline is missing.' }
    $bypass = Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='effect_bypass';effect=0;enabled=(-not $effect.bypass)}
    if ($bypass.sounds[0].effects[0].bypass -eq $effect.bypass) { throw 'Native effect bypass did not change with VST3 attached.' }
    Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='effect_bypass';effect=0;enabled=[bool]$effect.bypass} | Out-Null
    $value = if ($effect.parameters[0] -lt 0.5) { 0.75 } else { 0.25 }
    $edited = Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='effect_parameter';effect=0;parameter=0;value=$value}
    if ([Math]::Abs($edited.sounds[0].effects[0].parameters[0] - $value) -gt 0.000001) { throw 'Native effect parameter did not change with VST3 attached.' }
    Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='effect_parameter';effect=0;parameter=0;value=$effect.parameters[0]} | Out-Null
    $after = Invoke-McpTool $session gp_audio_track @{document=$document;track=0;operation='state'}
    if ((Json $before.sounds) -ne (Json $after.sounds)) { throw 'Native chain changed after restoring its bypass and parameter.' }
    $result.native_effect_operations = @{before=$before;bypass=$bypass;edited=$edited;after=$after}
}

try {
    $programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    $vst3Paths = @($Vst3Root -split ';' | Where-Object { $_ } | ForEach-Object {
        if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $programFiles ('Common Files/VST3/' + $_) }
    })
    $environment = @{
        GPVST3_ENABLE_P2_EFFECT = '0'
        GPVST3_RUNTIME_VST3 = $vst3Paths[0]
        GPVST3_VST3_ROOT = $vst3Paths -join ';'
    }
    if ($HookMode -eq 'enabled') { $environment.GPVST3_ENABLE_P2_HOOK = '1' }
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -McpRoot $McpRoot -Environment $environment
    $statusPath = Join-Path $dataDirectory 'status.json'; $sessionPath = Join-Path $run 'mcp/native-session.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200; $process.Refresh(); if ($process.HasExited) { throw 'Guitar Pro exited before P8 startup.' }
    }
    if (-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) { throw 'P8 startup files were not published.' }
    $result.identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
    $session = New-McpSession -SessionFile $sessionPath
    $seedSource = Join-Path $McpRoot 'test/testdata/minimal.gp'
    $seedPath = Join-Path $run 'seed.gp'
    Copy-Item -LiteralPath $seedSource -Destination $seedPath
    $seed = Invoke-McpTool $session gp_open @{path=$seedPath}
    Wait-Operation $seed.request 'opened' | Out-Null
    $created = Invoke-McpTool $session gp_new @{template='Steel Guitar'}
    $document = (Wait-Operation $created.request 'created').document
    $result.document = $document
    Start-Sleep -Milliseconds 700
    Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
    $inserted = Invoke-McpTool $session gp_insert_track @{document=$document;source_track=0;copy_content=$true}
    $result.inserted_track = $inserted
    if ($inserted.inserted_track -ne 1) { throw "Second RSE track was not inserted: $(Json $inserted)" }
    foreach ($track in @(0,1)) {
        $riff = Invoke-McpTool $session gp_insert_tab @{document=$document;track=$track;string=0;bar=0;text='0-2-5-7';mode='replace';denominator=4}
        Wait-Operation $riff.request 'applied' | Out-Null
    }
    $fixturePath = Join-Path $run 'two-tracks.gp'
    Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
    Start-Sleep -Milliseconds 500
    $mapping = Invoke-McpTool $session gp_audio_abi @{document=$document;operation='state'}
    $result.mapping = $mapping
    $tracks = @($mapping.tracks)
    if ($mapping.status -ne 'verified' -or $tracks.Count -lt 2) { throw "P8 mapping did not expose two verified tracks: $(Json $mapping)" }
    if ($tracks[0].track_id -eq $tracks[1].track_id -or @($tracks | Where-Object { $_.chains.Count -eq 0 }).Count) { throw "Track IDs/chains are not distinct: $(Json $mapping)" }

    $scanDeadline = [DateTime]::UtcNow.AddSeconds(90)
    do {
        $startup = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        if (-not $startup.vst3_host.scan_pending -and -not $startup.vst3_host.recognition_pending -and $startup.vst3_host.status -ne 'scanning') { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $scanDeadline)
    $result.startup = $startup
    if ($startup.vst3_host.scan_pending -or $startup.vst3_host.recognition_pending -or $startup.vst3_host.status -eq 'scanning') { throw 'P8 VST3 catalog scan did not complete.' }
    $vst3Paths = @($environment.GPVST3_VST3_ROOT -split ';')
    $candidates = @()
    foreach ($path in $vst3Paths) {
        $candidate = @($startup.vst3_catalog | Where-Object { [IO.Path]::GetFullPath($_.module) -ieq [IO.Path]::GetFullPath($path) })[0]
        if (-not $candidate) { throw "P8 static candidate missing: $path" }
        if (-not $candidate.class_id) {
            $sha = [Security.Cryptography.SHA256]::Create(); try { $token = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($candidate.module)))).Replace('-','').ToLower().Substring(0,16) } finally { $sha.Dispose() }
            $identify = Invoke-McpTool $session gp_objects @{query=('gpvst3Identify_' + $token);limit=10}
            $target = @($identify.objects)[0]
            if (-not $target) { throw "Identification control missing: $path" }
            Invoke-McpTool $session gp_set_property @{snapshot=$identify.snapshot;id=$target.id;property='checked';value=$true} | Out-Null
            Start-Sleep -Seconds 2
            $saved = Get-Content -LiteralPath (Join-Path $dataDirectory 'effect-chain.json') -Raw | ConvertFrom-Json
            $candidate = @($saved.effects | Where-Object { [IO.Path]::GetFullPath($_.module) -ieq [IO.Path]::GetFullPath($path) -and $_.class_id })[0]
        }
        if (-not $candidate.class_id) { throw "VST3 identification failed: $path" }
        $candidates += $candidate
    }
    $result.candidates = $candidates
    $result.native_audio_before = @(0,1 | ForEach-Object { Invoke-McpTool $session gp_audio_track @{document=$document;track=$_;operation='state'} })
    Set-TrackEffect 0 $candidates[0] | Out-Null
    Set-TrackEffect 1 $candidates[$candidates.Count - 1] | Out-Null
    if ($CheckGain) { $result.untitled_instances = @(Read-Gain 0; Read-Gain 1) }
    $save = Invoke-McpTool $session gp_save_as @{document=$document;path=$fixturePath}
    Wait-Operation $save.request 'saved' | Out-Null
    if ($CheckGain) {
        $result.named_instances = @(Read-Gain 0; Read-Gain 1)
        foreach ($track in @(0,1)) {
            if ($result.named_instances[$track].instance -ne $result.untitled_instances[$track].instance) { throw "First save rebuilt Track $track processor." }
        }
    }
    Assert-NativeLayout
    $result.native_audio_after = @(0,1 | ForEach-Object { Invoke-McpTool $session gp_audio_track @{document=$document;track=$_;operation='state'} })
    foreach ($track in @(0,1)) {
        if ((Json $result.native_audio_before[$track].sounds) -ne (Json $result.native_audio_after[$track].sounds)) {
            throw "Attaching VST3 changed the native sound/effect chain for track $track."
        }
    }
    if ($CheckLifecycle) { Assert-NativeEffects; Start-Sleep -Milliseconds 600 }
    $result.before_play = Get-Observation
    $initialPlayback = Invoke-McpTool $session gp_playback @{operation='state';document=$document}
    Invoke-McpTool $session gp_playback @{operation='set_loop';document=$document;enabled=$true} | Out-Null
    $play = Invoke-McpTool $session gp_playback @{operation='play';document=$document}
    Start-Sleep -Seconds 3
    $result.play = $play
    $result.observation = Get-Observation
    $hook = $result.observation.gp_hook
    if ($ExpectedBindingSource -and $hook.track_binding_source -ne $ExpectedBindingSource) {
        throw "Unexpected native binding provider: $($hook.track_binding_source)"
    }
    $result.track_runtime_evidence = $hook.track_runtime_evidence
    if ($hook.track_bindings_published -lt 2 -or -not $hook.track_context_stable -or $hook.track_scope_unresolved -or
        -not $hook.track_runtime_processed -or (-not $CheckGain -and -not $hook.track_runtime_write_observed) -or @($hook.track_runtime_evidence).Count -lt 2) {
        throw "Two-track runtime evidence is incomplete: $(Json $hook)"
    }
    foreach ($evidence in @($hook.track_runtime_evidence)) {
        if (-not $evidence.processed -or (-not $CheckGain -and -not $evidence.write_observed) -or $evidence.processed_blocks -lt 1) {
            throw "Track runtime did not process/write back: $(Json $evidence)"
        }
    }
    if ($CheckGain) {
        $beforeGain = @(Read-Gain 0; Read-Gain 1)
        foreach ($track in @(0,1)) {
            Read-Gain $track | Out-Null
            $gain = Invoke-McpTool $session gp_objects @{query='gpvst3TestGain';limit=10}
            $value = if ($track -eq 0) {0.25} else {0.5}
            Invoke-McpTool $session gp_set_property @{snapshot=$gain.snapshot;id=@($gain.objects)[0].id;property='value';value=$value} | Out-Null
            Start-Sleep -Milliseconds 400
        }
        $afterGain = @(Read-Gain 0; Read-Gain 1)
        $result.gain_before = $beforeGain; $result.gain_after = $afterGain
        if ($afterGain[0].instance -eq $afterGain[1].instance) { throw 'Two tracks share a processor instance.' }
        foreach ($track in @(0,1)) {
            $value = if ($track -eq 0) {0.25} else {0.5}
            $gain = $afterGain[$track]
            if ($gain.instance -ne $beforeGain[$track].instance -or $gain.gain -ne $value -or
                $gain.input_energy -le 0.000001 -or [Math]::Abs($gain.output_energy / $gain.input_energy - $value * $value) -gt 0.00001) {
                throw "Track $track independent gain evidence failed: $(Json $gain)"
            }
        }
        Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null
        Invoke-McpTool $session gp_playback @{operation='play';document=$document} | Out-Null
        Start-Sleep -Seconds 1
        $afterRestart = @(Read-Gain 0; Read-Gain 1)
        $result.gain_after_stop_play = $afterRestart
        foreach ($track in @(0,1)) {
            if ($afterRestart[$track].instance -ne $afterGain[$track].instance -or
                $afterRestart[$track].gain -ne $afterGain[$track].gain -or $afterRestart[$track].blocks -le $afterGain[$track].blocks) {
                throw "Track $track changed instance/state after stop/play."
            }
        }
        $global = Invoke-McpTool $session gp_objects @{query=('gpvst3GlobalEnabled_' + $candidates[0].class_id);limit=10}
        Invoke-McpTool $session gp_set_property @{snapshot=$global.snapshot;id=$global.objects[0].id;property='checked';value=$true} | Out-Null
        Start-Sleep -Milliseconds 500
        Read-Gain -1 | Out-Null
        $gain = Invoke-McpTool $session gp_objects @{query='gpvst3TestGain';limit=10}
        Invoke-McpTool $session gp_set_property @{snapshot=$gain.snapshot;id=$gain.objects[0].id;property='value';value=0.75} | Out-Null
        Start-Sleep -Milliseconds 500
        $globalGain = Read-Gain -1
        $trackGains = @(Read-Gain 0; Read-Gain 1)
        $globalAfterSwitch = Read-Gain -1
        $result.global_gain = $globalGain
        $result.tracks_with_global = $trackGains
        if ($globalGain.instance -in @($trackGains.instance) -or $globalAfterSwitch.instance -ne $globalGain.instance -or
            $globalGain.gain -ne 0.75 -or $globalGain.input_energy -le 0.000001 -or
            [Math]::Abs($globalGain.output_energy / $globalGain.input_energy - 0.5625) -gt 0.00001) {
            throw 'Global processor is not independent or does not process the Master buffer with its own gain.'
        }
        foreach ($track in @(0,1)) {
            if ($trackGains[$track].instance -ne $afterGain[$track].instance -or $trackGains[$track].gain -ne $afterGain[$track].gain) {
                throw "Enabling global changed Track $track processor/state."
            }
        }
        $result.observation = Get-Observation
        $finalTracks = @($result.observation.gp_hook.track_runtime_evidence)
        if ($finalTracks.Count -ne 2 -or @($finalTracks | Where-Object {
            -not $_.write_observed -or -not $_.processed -or $_.error_blocks -gt 0
        }).Count) { throw 'Final per-track processing/writeback evidence failed.' }
        if ($CheckLifecycle) {
            Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null
            Invoke-McpTool $session gp_edit_tracks @{operation='swap';document=$document;track=0;other=1} | Out-Null
            Start-Sleep -Milliseconds 600
            $swapped = @(Read-Gain 0; Read-Gain 1)
            $result.swapped = $swapped
            if ($swapped[0].instance -ne $trackGains[1].instance -or $swapped[0].gain -ne 0.5 -or
                $swapped[1].instance -ne $trackGains[0].instance -or $swapped[1].gain -ne 0.25) {
                throw 'Track swap reassigned or recreated a processor instead of following native track identity.'
            }
            Invoke-McpTool $session gp_edit_tracks @{operation='duplicate';document=$document;track=0} | Out-Null
            Start-Sleep -Milliseconds 600
            $duplicated = Get-Observation
            $result.duplicated = $duplicated.gp_hook.track_runtime_evidence
            if (@($result.duplicated).Count -ne 3 -or @($result.duplicated | Where-Object configured_effects -eq 0).Count -ne 1) {
                throw 'Inserted track inherited an existing track processor.'
            }
            $shifted = @(Read-Gain 0; Read-Gain 2)
            if ($shifted[0].instance -ne $swapped[0].instance -or $shifted[1].instance -ne $swapped[1].instance) { throw 'Inserting a track changed existing instances.' }
            Invoke-McpTool $session gp_edit_tracks @{operation='remove';document=$document;track=1} | Out-Null
            Start-Sleep -Milliseconds 400
            Invoke-McpTool $session gp_undo_redo @{operation='undo';document=$document} | Out-Null
            Start-Sleep -Milliseconds 400
            $result.after_delete_undo = (Get-Observation).gp_hook.track_runtime_evidence
            if (@($result.after_delete_undo).Count -ne 3) { throw 'Deleted track did not return after undo.' }
            Invoke-McpTool $session gp_undo_redo @{operation='redo';document=$document} | Out-Null
            Start-Sleep -Milliseconds 400
            Invoke-McpTool $session gp_edit_tracks @{operation='swap';document=$document;track=0;other=1} | Out-Null
            Start-Sleep -Milliseconds 500
            $renamedPath = Join-Path $run 'renamed-tracks.gp'
            $save = Invoke-McpTool $session gp_save_as @{document=$document;path=$renamedPath}
            Wait-Operation $save.request 'saved' | Out-Null
            $result.after_save_as = @(Read-Gain 0; Read-Gain 1)
            foreach ($track in @(0,1)) {
                if ($result.after_save_as[$track].instance -ne $trackGains[$track].instance) { throw 'Save As changed a live processor.' }
            }
            $closed = Invoke-McpTool $session gp_close @{document=$document;unsaved='reject'}
            Wait-Operation $closed.request 'closed' | Out-Null
            Start-Sleep -Milliseconds 600
            $open = Invoke-McpTool $session gp_open @{path=$renamedPath}
            $document = (Wait-Operation $open.request 'opened').document
            Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
            Wait-Track 0 | Out-Null
            Invoke-McpTool $session gp_playback @{operation='play';document=$document} | Out-Null
            Start-Sleep -Seconds 1
            $result.reopen_before_visiting_track_1 = (Get-Observation).gp_hook.track_runtime_evidence
            if (@($result.reopen_before_visiting_track_1).Count -ne 2 -or
                @($result.reopen_before_visiting_track_1 | Where-Object { $_.configured_effects -ne 1 -or -not $_.processed -or $_.error_blocks -gt 0 }).Count) {
                throw 'Reopening did not automatically restore and process both tracks before visiting their UI.'
            }
            $result.reopened_gains = @(Read-Gain 0; Read-Gain 1)
            if ($result.reopened_gains[0].gain -ne 0.25 -or $result.reopened_gains[1].gain -ne 0.5 -or
                (Read-Gain -1).instance -ne $globalGain.instance) { throw 'Reopening lost track state or changed the global instance.' }
            Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null
            Request-Gpvst3McpHostExit $session $process
            try { Close-McpSession $session } catch { }
            $session = $null
            $firstExit = Join-Path $run 'before-restart'
            New-Item -ItemType Directory -Force -Path $firstExit | Out-Null
            Stop-Gpvst3TestHost $process -RunDirectory $firstExit
            $restartRun = Join-Path $run 'restart'
            $environment.GPVST3_DATA_DIR = $dataDirectory
            $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $restartRun -McpRoot $McpRoot -Environment $environment
            $restartSessionPath = Join-Path $restartRun 'mcp/native-session.json'
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            do {
                Start-Sleep -Milliseconds 150
                $currentStatus = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
            } while (($currentStatus.pid -ne $process.Id -or -not (Test-Path -LiteralPath $restartSessionPath)) -and [DateTime]::UtcNow -lt $deadline)
            $result.restart_identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
            $session = New-McpSession -SessionFile $restartSessionPath
            $open = Invoke-McpTool $session gp_open @{path=$renamedPath}
            $document = (Wait-Operation $open.request 'opened').document
            Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
            Wait-Track 0 | Out-Null
            Invoke-McpTool $session gp_playback @{operation='play';document=$document} | Out-Null
            Start-Sleep -Seconds 2
            $result.restart_before_visiting_track_1 = (Get-Observation).gp_hook
            if (@($result.restart_before_visiting_track_1.track_runtime_evidence).Count -ne 2 -or
                @($result.restart_before_visiting_track_1.track_runtime_evidence | Where-Object { $_.configured_effects -ne 1 -or -not $_.processed -or $_.error_blocks -gt 0 }).Count -or
                -not $result.restart_before_visiting_track_1.global_chain_enabled) { throw 'Process restart did not restore all track and global chains.' }
            $result.restart_gains = @(Read-Gain 0; Read-Gain 1; Read-Gain -1)
            if ($result.restart_gains[0].gain -ne 0.25 -or $result.restart_gains[1].gain -ne 0.5 -or $result.restart_gains[2].gain -ne 0.75) {
                throw 'Process restart lost an independent processor state.'
            }
        }
    }
    $result | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P8 two-track VST3 runtime mapping, processing and writeback. Evidence: $run"
}
catch {
    $result.failure = $_.Exception.Message
    $result.failure_stack = $_.ScriptStackTrace
    $result | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    throw
}
finally {
    if ($session) {
        try {
            if ($initialPlayback) { Invoke-McpTool $session gp_playback @{operation='stop';document=$document} | Out-Null }
            if (-not $KeepHost) { Request-Gpvst3McpHostExit $session $process; Start-Sleep -Milliseconds 500 }
        } catch { Write-Warning $_.Exception.Message }
        try { Close-McpSession $session } catch {}
    }
    try { Stop-Gpvst3TestHost $process -KeepHost:$KeepHost -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
