param(
    [Parameter(Mandatory=$true)][string]$ScorePath,
    [Parameter(Mandatory=$true)][string[]]$Vst3Module,
    [Parameter(Mandatory=$true)][string[]]$ClassId,
    [string]$InitialSidecar = '',
    [int]$Track = 0,
    [string]$PluginPath = '',
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/preload-retention-fix-build/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if ($Vst3Module.Count -ne 1 -and $Vst3Module.Count -ne $ClassId.Count) { throw 'Supply one module or one module per class ID.' }
foreach ($path in (@($ScorePath,$PluginPath) + $Vst3Module)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing prerequisite: $path" }
}
. (Join-Path $PSScriptRoot 'host-session.ps1')
. (Join-Path $McpRoot 'native/mcp-client.ps1')
$run = Join-Path $root ('artifacts/preload-mcp-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$score = Join-Path $run ([IO.Path]::GetFileName($ScorePath))
Copy-Item -LiteralPath $ScorePath -Destination $score
$scoreKey = $score.Replace('\','/')
$modules = @($Vst3Module | ForEach-Object { (Resolve-Path -LiteralPath $_).Path.Replace('\','/').ToLowerInvariant() })
$module = $modules[0]
$saved = $null
if ($InitialSidecar) { $saved = Get-Content -LiteralPath $InitialSidecar -Raw | ConvertFrom-Json }
$candidates = if ($saved) { @($saved.global.effects) + @($saved.scores.PSObject.Properties | ForEach-Object {
    $_.Value.tracks.PSObject.Properties | ForEach-Object { $_.Value.effects }
}) } else { @() }
# Only the first plugin has saved state; all others must still preload.
$source = $candidates | Where-Object { $_.class_id -eq $ClassId[0] -and $_.component_state } | Select-Object -First 1
$row = @{module=$module;class_id=$ClassId[0];enabled=$false;bypass=$true;desired_enabled=$true;configured=$true;identified=$true;order=0}
if ($source) { $row.component_state = $source.component_state; $row.controller_state = $source.controller_state }
$effects = @($row)
@{schema=2;global=@{effects=@($effects)};effects=@($effects);scores=@{
    $scoreKey=@{tracks=@{'preload-track'=@{track_index=$Track;present=$true;effects=@($effects)}}}
}} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $run 'effect-chain.json') -Encoding UTF8
$observationPath = Join-Path $run 'p2-observation.json'
$session=$null; $process=$null; $document=$null
$evidence=[ordered]@{schema=1;run=$run;module=$module;class_id=$ClassId;track=$Track;hook_mode='default'}
$before = Get-Gpvst3HostSnapshot $HostDirectory
function Wait-Observation([scriptblock]$Check,[string]$Stage,[int]$Seconds=30) {
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if (Test-Path -LiteralPath $observationPath) {
            $value=Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
            if (& $Check $value.gp_hook) { return $value.gp_hook }
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    $evidence.last_observation=$value
    throw "Preload test timed out at $Stage"
}
function Wait-Operation([string]$Request,[string]$Expected) {
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        $state=Invoke-McpTool $session gp_operation @{request=$Request}
        if ($state.operation.status -eq $Expected) { return $state.operation }
        if ($state.operation.status -in @('error','cancelled')) { throw "Operation failed: $($state | ConvertTo-Json -Depth 8 -Compress)" }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Operation did not reach $Expected"
}
function Find-Object([string]$Name) {
    $query=Invoke-McpTool $session gp_objects @{query=$Name;limit=30}
    $object=@($query.objects | Where-Object object_name -eq $Name | Sort-Object visible -Descending)[0]
    if (-not $object) { throw "Missing control $Name" }
    return @{snapshot=$query.snapshot;id=$object.id;object=$object}
}
function Set-Effect([string]$Scope,[bool]$Enabled,[int]$Index=0) {
    $prefix=if($Scope -eq 'global'){'gpvst3GlobalEnabled_'}else{'gpvst3Enabled_'}
    $target=Find-Object ($prefix+$ClassId[$Index])
    Invoke-McpTool $session gp_set_property @{snapshot=$target.snapshot;id=$target.id;property='checked';value=$Enabled} | Out-Null
}
try {
    $environment=@{GPVST3_VST3_ROOT=($modules -join ';');GPVST3_DIAGNOSTIC_MODE='detailed'}
    $process=Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -McpRoot $McpRoot -Environment $environment
    $sessionPath=Join-Path $run 'mcp/native-session.json';$statusPath=Join-Path $run 'status.json'
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    while ((-not(Test-Path $sessionPath) -or -not(Test-Path $statusPath)) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    $evidence.identity=Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
    $session=New-McpSession -SessionFile $sessionPath
    $opened=Invoke-McpTool $session gp_open @{path=$score}
    $document=(Wait-Operation $opened.request 'opened').document
    $evidence.document=$document
    Invoke-McpTool $session gp_activate @{document=$document} | Out-Null
    $preloaded=Wait-Observation {param($h)
        -not $h.preload_pending -and $h.global_preloaded -and -not $h.input_preloaded -and
        $h.track_preloaded -ge 1 -and @($h.instances | Where-Object scope -eq 'global').Count -eq $ClassId.Count -and
        @($h.instances | Where-Object scope -eq 'track').Count -ge $ClassId.Count
    } 'preload every catalog plugin' 120
    $evidence.preloaded=$preloaded
    if (@($preloaded.instances | Where-Object { $_.active -or $_.processed_blocks -ne 0 -or -not $_.preloaded }).Count) { throw 'Preload activated or processed an unselected effect.' }
    if (@($preloaded.instances.instance_id | Sort-Object -Unique).Count -ne @($preloaded.instances).Count) { throw 'Preloaded scopes share a processor instance.' }
    if ($preloaded.input_route -ne 'disabled' -or $preloaded.input_route_enabled -or @($preloaded.instances | Where-Object scope -eq 'input').Count) { throw 'Preloading opened the input monitor.' }
    $sidecar=Get-Content -LiteralPath (Join-Path $run 'effect-chain.json') -Raw | ConvertFrom-Json
    if (@($sidecar.global.effects | Where-Object enabled).Count -or @($sidecar.scores.PSObject.Properties | ForEach-Object {
        $_.Value.tracks.PSObject.Properties | ForEach-Object { $_.Value.effects | Where-Object enabled }
    }).Count) { throw 'Preload changed persisted activation.' }
    Invoke-McpTool $session gp_window @{state='restore'} | Out-Null
    $tab=Find-Object 'tabTrackButton'
    if (-not $tab.object.properties.checked) { Invoke-McpTool $session gp_trigger @{snapshot=$tab.snapshot;id=$tab.id} | Out-Null }
    Invoke-McpTool $session gp_cursor @{document=$document;axis='track';index=$Track} | Out-Null
    $deadline=[DateTime]::UtcNow.AddSeconds(8)
    do {
        $context=Invoke-McpTool $session gp_objects @{query='gpvst3TrackContext';limit=10}
        $label=@($context.objects | Where-Object parent_name -eq 'gpvst3P7Panel')[0]
        if ($label.properties.text -eq "当前音轨：Track $Track") { break }
        Start-Sleep -Milliseconds 100
    } while([DateTime]::UtcNow -lt $deadline)
    if ($label.properties.text -ne "当前音轨：Track $Track") { throw 'Track scope did not follow the selected track.' }
    $initialPlayback=Invoke-McpTool $session gp_playback @{document=$document;operation='state'}
    Invoke-McpTool $session gp_playback @{document=$document;operation='set_loop';enabled=$true} | Out-Null
    Invoke-McpTool $session gp_playback @{document=$document;operation='play'} | Out-Null
    $dormant=Wait-Observation {param($h) $h.audio_output_callback.call_count -gt $preloaded.audio_output_callback.call_count+20} 'playback while bypassed'
    if (@($dormant.instances | Where-Object { $_.processed_blocks -ne 0 -or $_.active }).Count) { throw 'Preloaded processor ran while bypassed.' }
    $evidence.dormant_playback=$dormant
    $enableTimer=[Diagnostics.Stopwatch]::StartNew()
    Set-Effect 'track' $true
    $trackActive=Wait-Observation {param($h) @($h.instances | Where-Object { $_.scope -eq 'track' -and $_.active -and $_.processed_blocks -gt 0 }).Count -eq 1} 'enable preloaded track'
    $evidence.track_enable_observed_ms=$enableTimer.Elapsed.TotalMilliseconds
    $targetTrackKey=($trackActive.instances | Where-Object { $_.scope -eq 'track' -and $_.active }).track_key
    $evidence.track_active=$trackActive
    $tab=Find-Object 'tabScoreButton'
    if (-not $tab.object.properties.checked) { Invoke-McpTool $session gp_trigger @{snapshot=$tab.snapshot;id=$tab.id} | Out-Null }
    Set-Effect 'global' $true
    $globalActive=Wait-Observation {param($h) @($h.instances | Where-Object { $_.scope -eq 'global' -and $_.active -and $_.processed_blocks -gt 0 }).Count -eq 1 -and -not $h.input_route_enabled} 'enable global without input monitoring'
    $evidence.all_active=$globalActive
    $switches=@()
    foreach ($scope in @('global','track')) {
        $tab=Find-Object $(if($scope -eq 'global'){'tabScoreButton'}else{'tabTrackButton'})
        if (-not $tab.object.properties.checked) { Invoke-McpTool $session gp_trigger @{snapshot=$tab.snapshot;id=$tab.id} | Out-Null }
        $previous=0
        $order=if($ClassId.Count -gt 1){@(1..($ClassId.Count-1)) + @(0)}else{@(0)}
        foreach($index in $order) {
            Set-Effect $scope $false $previous
            $bypassed=Wait-Observation {param($h)
                @($h.instances | Where-Object { $_.scope -eq $scope -and $_.active -and
                    ($scope -eq 'global' -or $_.track_key -eq $targetTrackKey) }).Count -eq 0
            } "disable $scope plugin $previous"
            Set-Effect $scope $true $index
            $reactivated=Wait-Observation {param($h)
                @($h.instances | Where-Object { $_.scope -eq $scope -and $_.class_id -eq $ClassId[$index] -and
                    $_.active -and $_.processed_blocks -gt 0 -and
                    ($scope -eq 'global' -or $_.track_key -eq $targetTrackKey) }).Count -eq 1
            } "enable $scope plugin $index"
            foreach($instance in $preloaded.instances) {
                $current=@($reactivated.instances | Where-Object {
                    $_.scope -eq $instance.scope -and $_.track_key -eq $instance.track_key -and
                    $_.module -eq $instance.module -and $_.class_id -eq $instance.class_id
                })
                if ($current.Count -ne 1 -or $current[0].instance_id -ne $instance.instance_id) { throw 'Switching discarded or duplicated a preloaded instance.' }
            }
            if ($reactivated.input_route_enabled -or $reactivated.input_processor_ready -or $reactivated.input_processed_blocks -gt 0) { throw 'Global selection unexpectedly monitored the input device.' }
            $switches+=@{scope=$scope;index=$index;observation=$reactivated}
            $previous=$index
        }
    }
    $evidence.switches=$switches
    $evidence.result='pass'
    Write-Output "PASS: MCP preloads every catalog plugin, retains instances across switches and keeps input monitoring disabled. Evidence: $run"
} catch {
    $evidence.result='fail';$evidence.failure=$_.Exception.Message;$evidence.stack=$_.ScriptStackTrace
    throw
} finally {
    $evidence | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    if($session) {
        try {
            if($document) {
                Invoke-McpTool $session gp_playback @{document=$document;operation='stop'} | Out-Null
                $closed=Invoke-McpTool $session gp_close @{document=$document;unsaved='reject'}
                Wait-Operation $closed.request 'closed' | Out-Null
            }
            Request-Gpvst3McpHostExit $session $process
        } catch { Write-Warning $_.Exception.Message }
        try { Close-McpSession $session } catch {}
    }
    try { Stop-Gpvst3TestHost $process -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
