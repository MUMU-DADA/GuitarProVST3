param(
    [Parameter(Mandatory=$true)][string]$CollectionPath,
    [string]$OutputPath = ''
)

# Offline diagnostics only: short callback excerpts are not continuous PCM,
# physical loopback measurements or proof that the P13 host contract passes.
$ErrorActionPreference = 'Stop'
$CollectionPath = (Resolve-Path -LiteralPath $CollectionPath).Path
if (-not $OutputPath) { $OutputPath = Join-Path (Split-Path -Parent $CollectionPath) 'probe-analysis.json' }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ($OutputPath -ieq $CollectionPath) { throw 'Analysis output must not overwrite the collection evidence.' }
$collection = Get-Content -LiteralPath $CollectionPath -Raw -Encoding UTF8 | ConvertFrom-Json
$probe = $collection.probe
$probeSource = 'collection.probe'
if (-not $probe) {
    $tracePath = Join-Path (Split-Path -Parent $CollectionPath) 'data/p13-input-probe.json'
    if (Test-Path -LiteralPath $tracePath -PathType Leaf) {
        $probe = Get-Content -LiteralPath $tracePath -Raw -Encoding UTF8 | ConvertFrom-Json
        $probeSource = 'data/p13-input-probe.json; collection snapshot unavailable'
    }
}
if (-not $probe -or -not $probe.records) { throw 'No published P13 records are available to analyze.' }
$records = @($probe.records | Sort-Object { [uint64]$_.index })

function Get-P13Distribution($Items, [string]$Property) {
    return @($Items | Group-Object -Property $Property | Sort-Object Name | ForEach-Object {
        [ordered]@{value=$(if ($_.Name -ceq '') { $null } else { $_.Name });count=$_.Count}
    })
}

function Get-P13Timing($Items, [string]$Property) {
    $values = @($Items | ForEach-Object { [double]$_.$Property } | Sort-Object)
    if (-not $values.Count) { return $null }
    return [ordered]@{
        p50_ns=$values[[int][Math]::Floor(($values.Count - 1) / 2)]
        p95_ns=$values[[int][Math]::Ceiling($values.Count * 0.95) - 1]
        max_ns=$values[-1]
    }
}

function Get-P13SampleSummary($Items, [string]$Property) {
    $captured = 0
    $nonzeroRecords = 0
    $storedNonfinite = 0
    $maskedNonfiniteRecords = 0
    $sampleCount = 0
    $peak = 0.0
    $sumSquares = 0.0
    $shapeErrors = 0
    foreach ($record in $Items) {
        $samples = $record.$Property
        if (-not $samples -or $samples.status -ne 'captured') { continue }
        ++$captured
        if ([uint64]$samples.non_finite_mask -ne 0) { ++$maskedNonfiniteRecords }
        $values = @($samples.values)
        if ($values.Count -ne [int]$samples.frames * [int]$samples.channels -or
            [int]$samples.frames -gt [int]$record.frames -or [int]$samples.frames -lt 1 -or
            [int]$samples.channels -notin @(1, 2)) { ++$shapeErrors }
        $hasNonzero = $false
        foreach ($raw in $values) {
            $value = [double]$raw
            if ($null -eq $raw -or [double]::IsNaN($value) -or [double]::IsInfinity($value)) { ++$storedNonfinite; continue }
            ++$sampleCount
            if ($value -ne 0) { $hasNonzero = $true }
            $peak = [Math]::Max($peak, [Math]::Abs($value))
            $sumSquares += $value * $value
        }
        if ($hasNonzero) { ++$nonzeroRecords }
    }
    return [ordered]@{
        statuses=@($Items | ForEach-Object { $_.$Property } | Group-Object status | ForEach-Object { [ordered]@{status=$_.Name;count=$_.Count} })
        captured_records=$captured
        nonzero_excerpt_records=$nonzeroRecords
        finite_stored_sample_count=$sampleCount
        stored_nonfinite_samples=$storedNonfinite
        records_with_original_nonfinite_mask=$maskedNonfiniteRecords
        sample_shape_errors=$shapeErrors
        excerpt_peak=$peak
        excerpt_rms=$(if ($sampleCount) { [Math]::Sqrt($sumSquares / $sampleCount) } else { $null })
        continuous_audio=$false
        note='Statistics cover only the stored short excerpts; zeroed non-finite samples are represented by non_finite_mask.'
    }
}

function Get-P13ListenerSummary($Probe) {
    $listener=$Probe.listener_probe
    $items=@($listener.records | Where-Object { $null -ne $_ })
    $parents=@{}
    foreach ($record in $Probe.records) { $parents[[string]$record.sequence]=$record }
    $missing=0; $outside=0; $threadMismatch=0; $unknownEnabled=0; $unstable=0
    $outputChanged=0; $uncomparableOutput=0; $peakInvalid=0
    $sinkSamples=@(); $sinkBusy=0; $sinkUnknown=0
    foreach ($record in $items) {
        if ($record.unit_enabled_at_entry -isnot [bool]) { ++$unknownEnabled }
        if ($record.state_pointer_stable -ne $true) { ++$unstable }
        if ($record.peak_nonfinite_mask -ne 0) { ++$peakInvalid }
        if ($record.sink_busy -eq $true) { ++$sinkBusy }
        if ($listener.sink_requested -eq $true -and ($record.sink_applied -isnot [bool] -or $record.sink_busy -isnot [bool])) { ++$sinkUnknown }
        if ($record.sink_applied -eq $true) {
            $sinkSamples += [pscustomobject]@{frames=$record.frames;samples=[pscustomobject]@{
                status='captured';frames=[Math]::Min([int]$record.frames,16);channels=$record.output_channels
                non_finite_mask=$record.sink_nonfinite_mask;values=@($record.sink_output)
            }}
        }
        $parentSequence=[uint64]0
        if (-not [uint64]::TryParse([string]$record.callback_sequence,[ref]$parentSequence) -or $parentSequence -eq 0 -or -not $parents.ContainsKey([string]$parentSequence)) { ++$missing }
        else {
            $parent=$parents[[string]$parentSequence]
            if ($record.thread -ne $parent.thread) { ++$threadMismatch }
            $start=[uint64]$record.timestamp_ns; $end=$start+[uint64]$record.observed_ns
            if ($start -lt [uint64]$parent.timestamp_ns -or $end -gt ([uint64]$parent.timestamp_ns+[uint64]$parent.callback_ns)) { ++$outside }
        }
        $before=@($record.output_before); $after=@($record.output_after.values)
        if ($record.output_before_nonfinite_mask -ne 0 -or $record.output_after.non_finite_mask -ne 0 -or
            $record.output_after.status -ne 'captured' -or $before.Count -ne $after.Count) { ++$uncomparableOutput }
        else {
            for ($i=0;$i -lt $before.Count;++$i) {
                if ([double]$before[$i] -ne [double]$after[$i]) { ++$outputChanged; break }
            }
        }
    }
    $enabled=@($items | Where-Object unit_enabled_at_entry -EQ $true).Count
    $disabled=@($items | Where-Object unit_enabled_at_entry -EQ $false).Count
    $duplicates=$items.Count-@($items.sequence | Sort-Object -Unique).Count
    $complete=$listener.requested -eq $true -and $listener.installed -eq $true -and $listener.complete -eq $true -and
        [int]$Probe.capacity -gt 0 -and $items.Count -eq [int]$Probe.capacity -and [int]$listener.published -eq [int]$Probe.capacity
    $correlated=$complete -and -not ($missing+$outside+$threadMismatch+$unknownEnabled+$unstable+$duplicates)
    return [ordered]@{
        requested=$listener.requested; installed=$listener.installed; complete=$complete; records=$items.Count
        status=$(if (-not $listener) {'not_available'} elseif (-not $correlated) {'incomplete_or_inconsistent'} elseif ($enabled -eq 0) {'collected_inactive_listener'} else {'collected_unvalidated'})
        complete_and_correlated=$correlated
        enabled_records=$enabled; disabled_records=$disabled; unknown_enabled_records=$unknownEnabled
        active_listener_path_observed=($enabled -gt 0)
        callback_parent_field='callback_sequence; native Metadata.parentSequence'
        missing_parent_records=$missing; outside_parent_time_records=$outside; parent_thread_mismatches=$threadMismatch
        distinct_parent_sequences=@($items.callback_sequence|Sort-Object -Unique).Count
        duplicate_listener_sequences=$duplicates; unstable_state_records=$unstable
        frames=@(Get-P13Distribution $items 'frames')
        internal_sample_rates=@(Get-P13Distribution $items 'internal_sample_rate')
        unit_enabled_states=@(Get-P13Distribution $items 'unit_enabled_at_entry')
        returned_full_frame_states=@(Get-P13Distribution $items 'returned_full_frames')
        original_timing=(Get-P13Timing $items 'original_ns')
        observed_timing=(Get-P13Timing $items 'observed_ns')
        input_excerpts=(Get-P13SampleSummary $items 'input')
        output_after_excerpts=(Get-P13SampleSummary $items 'output_after')
        output_changed_excerpt_records=$outputChanged; uncomparable_output_excerpt_records=$uncomparableOutput
        output_unchanged_excerpt_records=($items.Count-$outputChanged-$uncomparableOutput)
        sink_requested=$listener.sink_requested
        sink_applied_records=$sinkSamples.Count; sink_busy_records=$sinkBusy; unknown_sink_state_records=$sinkUnknown
        sink_excerpts=(Get-P13SampleSummary $sinkSamples 'samples')
        sink_interpretation='A nonzero sink and unchanged caller excerpts support only bounded listener-output diversion; shared SRC/ring history, complete buffers, DSP equivalence and P13 acceptance remain unproven.'
        peak_nonfinite_records=$peakInvalid
        note='The listener can receive resampled input while disabled. Complete and correlated records do not prove active monitoring, tail isolation or RSE invariance; excerpts are not continuous PCM.'
    }
}

$sequences = @($records | ForEach-Object { [uint64]$_.sequence } | Sort-Object -Unique)
$missingSequences = [uint64]0
$gapCount = 0
for ($index = 1; $index -lt $sequences.Count; ++$index) {
    $gap = $sequences[$index] - $sequences[$index - 1] - 1
    if ($gap -gt 0) { ++$gapCount; $missingSequences += $gap }
}
$startNs = [uint64]::MaxValue
$endNs = [uint64]0
$deadlineMisses = 0
$unknownBudget = 0
$actualRate = [double]$probe.actual_asio_sample_rate
$actualRateUsable = $null -ne $probe.actual_asio_sample_rate_query_result -and
    $probe.actual_asio_sample_rate_query_result -eq 0 -and $actualRate -ge 8000 -and $actualRate -le 768000 -and
    -not [double]::IsNaN($actualRate) -and -not [double]::IsInfinity($actualRate) -and
    $collection.audio_configuration_unchanged -eq $true -and
    @($records.owner_address | Sort-Object -Unique).Count -eq 1 -and
    @($records.input_device | Sort-Object -Unique).Count -eq 1 -and
    @($records.output_device | Sort-Object -Unique).Count -eq 1
$timestampRegressions = 0
$lastTimestamp = [uint64]0
foreach ($record in $records) {
    $start = [uint64]$record.timestamp_ns
    $end = $start + [uint64]$record.callback_ns
    if ($start -lt $startNs) { $startNs = $start }
    if ($end -gt $endNs) { $endNs = $end }
    if ($lastTimestamp -gt $start) { ++$timestampRegressions }
    $lastTimestamp = $start
    # PaStreamInfo.sampleRate can be the requested 44100 while the ASIO device
    # actually runs at 192000. It is never a hardware-deadline denominator.
    if (-not $actualRateUsable -or $null -eq $record.host_api_type -or [int]$record.host_api_type -ne 3 -or [double]$record.frames -le 0) { ++$unknownBudget }
    elseif ([double]$record.callback_ns -gt ([double]$record.frames * 1e9 / $actualRate)) { ++$deadlineMisses }
}
$coverage = 'unverified_missing_playback_timestamps'
if ($collection.playback_confirmed_ns -and $collection.playback_last_confirmed_ns) {
    $coverage = if ($startNs -ge [uint64]$collection.playback_confirmed_ns -and $endNs -le [uint64]$collection.playback_last_confirmed_ns) {
        'bracketed_by_playing_observations'
    } else { 'not_fully_bracketed_by_playing_observations' }
}
$complete = $probe.enabled -eq $true -and $probe.complete -eq $true -and [int]$probe.capacity -gt 0 -and
    $records.Count -eq [int]$probe.capacity -and [int]$probe.published -eq [int]$probe.capacity
$shutdown = $collection.shutdown
if (-not $shutdown) {
    $shutdownPath = Join-Path (Split-Path -Parent $CollectionPath) 'shutdown.json'
    if (Test-Path -LiteralPath $shutdownPath) { $shutdown = Get-Content -LiteralPath $shutdownPath -Raw | ConvertFrom-Json }
}
$analysis = [ordered]@{
    schema=1
    status='analyzed_unvalidated'
    p13_acceptance='not_evaluated'
    source_collection=$CollectionPath
    source_sha256=(Get-FileHash -LiteralPath $CollectionPath -Algorithm SHA256).Hash
    probe_source=$probeSource
    collection_status=$collection.status
    probe_complete=$complete
    records=$records.Count
    recorder_capacity=$probe.capacity
    first_callback_ns=[string]$startNs
    last_measured_callback_end_ns=[string]$endNs
    sampled_callback_window_ms=([decimal]($endNs - $startNs) / 1000000)
    playback_window_coverage=$coverage
    continuous_playback_proven=$false
    callback_frames=@(Get-P13Distribution $records 'frames')
    requested_sample_rates=@(Get-P13Distribution $records 'sample_rate')
    requested_sample_rate_source='PaStreamInfo requested rate; including legacy records without sample_rate_source'
    actual_asio_sample_rate=$probe.actual_asio_sample_rate
    actual_asio_sample_rate_query_result=$probe.actual_asio_sample_rate_query_result
    actual_sample_rate_source='PaAsio_GetSampleRate control-thread snapshot; not sampled in each recorded callback'
    audio_configuration_observed_unchanged=$collection.audio_configuration_unchanged
    input_channels=@(Get-P13Distribution $records 'input_channels')
    output_channels=@(Get-P13Distribution $records 'output_channels')
    driver_frames=@(Get-P13Distribution $records 'driver_frames')
    driver_input_latency_samples=@(Get-P13Distribution $records 'driver_input_latency_samples')
    driver_output_latency_samples=@(Get-P13Distribution $records 'driver_output_latency_samples')
    host_api_types=@(Get-P13Distribution $records 'host_api_type')
    callback_status_flags=@(Get-P13Distribution $records 'status_flags')
    original_callback_results=@(Get-P13Distribution $records 'original_result')
    invalid_configuration_records=@($records | Where-Object configuration_valid -NE $true).Count
    alias_records=@($records | Where-Object pointers_alias -EQ $true).Count
    silence_experiment_records=@($records | Where-Object silence_experiment -EQ $true).Count
    sequence_gap_events=$gapCount
    missing_sequence_numbers=[string]$missingSequences
    duplicate_sequence_numbers=($records.Count - $sequences.Count)
    timestamp_regressions_in_record_index_order=$timestampRegressions
    callback_timing=(Get-P13Timing $records 'callback_ns')
    original_callback_timing=(Get-P13Timing $records 'original_ns')
    callback_budget_status=$(if ($unknownBudget -eq $records.Count) { 'deadline_unknown' } elseif ($unknownBudget) { 'partially_unknown' } else { 'conditional_control_rate_no_observed_device_change' })
    conditional_callback_budget_exceedances=$(if ($unknownBudget -lt $records.Count) { $deadlineMisses } else { $null })
    callback_budget_unknown_records=$unknownBudget
    legacy_probe_observed_deadline_misses_not_used=$probe.observed_deadline_misses
    timing_includes_probe_overhead=$probe.timing_includes_probe_overhead
    timing_excludes_recorder_publication=$probe.timing_excludes_recorder_publication
    input_excerpts=(Get-P13SampleSummary $records 'capture')
    post_original_excerpts=(Get-P13SampleSummary $records 'post_original')
    listener=(Get-P13ListenerSummary $probe)
    shutdown=$shutdown
    limitations=@(
        'Unknown driver_frames or host_api_type remain unknown; callback frames and configured device settings are not replacements.',
        'Input excerpts establish sampled nonzero values only, not a continuously captured signal, latency or isolated musical source.',
        'Sequence gaps may reflect recorder concurrency or omitted callbacks; they are not automatically hardware xruns.',
        'Callback budget comparisons include diagnostic overhead and exclude unmeasured work; they are not complete device-deadline evidence.',
        'Legacy probe observed_deadline_misses used the requested PaStreamInfo rate and is not reliable; this analyzer never uses it.',
        'The actual ASIO rate is a later control-thread query. Conditional budget comparisons require no observed configuration or stream change and still cannot prove the rate of each earlier callback.',
        'Playback polling cannot prove uninterrupted or audible RSE output; native-input isolation and RSE invariance remain unevaluated.'
    )
}
$analysis | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "P13 probe analysis: $OutputPath; acceptance=not_evaluated"
