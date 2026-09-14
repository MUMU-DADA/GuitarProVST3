# Shared acceptance gate for real VST3 output levels. Callback counts and
# writeback hashes are deliberately not inputs to this decision.
function Read-Gpvst3LevelObservation([string]$Path) {
    # QSaveFile replaces the file atomically. Permit rename/delete while the
    # reader holds the old snapshot so observation cannot obstruct its writer.
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
        try { $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}

function Get-Gpvst3LevelPoint($Observation, $Target) {
    $hook = $Observation.gp_hook
    if ($Target.scope -eq 'device') { $level = $hook.audio_output_level }
    elseif ($Target.scope -eq 'global') { $level = $hook.vst3_output_level }
    elseif ($Target.scope -eq 'track') {
        $tracks = @($hook.track_runtime_evidence | Where-Object track_key -EQ $Target.track_key)
        if ($tracks.Count -ne 1 -or $tracks[0].configured_effects -lt 1) {
            throw "Audio level target is not an active, uniquely identified track: $($Target.track_key)"
        }
        $level = $tracks[0].vst3_output_level
    } else { throw "Unsupported audio level scope: $($Target.scope)" }
    if (-not $level -or -not $hook.observation_nanoseconds) {
        throw 'Current audio level fields are missing. Build/load the updated DLL; old block/hash evidence cannot pass.'
    }
    if ($Target.module -and (-not $level.module -or
        [IO.Path]::GetFullPath($level.module) -ine [IO.Path]::GetFullPath($Target.module))) {
        throw "Audio level target mismatch: expected $($Target.module), observed $($level.module)."
    }
    $point = [ordered]@{
        instance = $level.instance; sequence = $level.sequence
        module = $level.module; class_id = $level.class_id
        sample_nanoseconds = $level.sample_nanoseconds
        observation_nanoseconds = $hook.observation_nanoseconds
        audio_generation = $hook.audio_generation; selection_request_id = $hook.selection_request_id
    }
    foreach ($name in @('input_valid','input_peak','input_rms','input_ac_rms',
                         'output_valid','output_peak','output_rms','output_ac_rms')) {
        $point[$name] = $level.$name
    }
    [pscustomobject]$point
}

function ConvertTo-Gpvst3Dbfs([double]$Amplitude) {
    20.0 * [Math]::Log10([Math]::Max($Amplitude, 0.000000000001))
}

function Get-Gpvst3LevelReadiness($Observation, [object[]]$Targets) {
    $hook = $Observation.gp_hook
    if ($hook.selection_status -eq 'failed') {
        throw "VST3 cold preparation failed: $($hook.runtime_effect_error) $($hook.track_runtime_error)"
    }
    if ($hook.selection_pending -or $hook.selection_status -in @('queued','preparing')) { return 'preparing' }
    foreach ($target in $Targets) {
        if ($target.scope -eq 'global' -and (-not $hook.runtime_processor_ready -or $hook.total_bypass)) { return 'preparing' }
        if ($target.scope -eq 'track') {
            $track = @($hook.track_runtime_evidence | Where-Object track_key -EQ $target.track_key)
            if ($track.Count -ne 1 -or -not $track[0].configured -or $track[0].configured_effects -lt 1) { return 'preparing' }
        }
        if ($target.scope -eq 'device' -and -not $hook.audio_output_callback_installed) { return 'waiting_for_callback' }
        $point = Get-Gpvst3LevelPoint $Observation $target
        if ($point.output_valid -ne $true -or $point.sequence -le 0 -or $point.sample_nanoseconds -le 0 -or
            $point.sample_nanoseconds -lt $hook.selection_committed_nanoseconds -or
            ($point.observation_nanoseconds - $point.sample_nanoseconds) / 1000000.0 -gt 750) {
            return 'waiting_for_callback'
        }
    }
    # Zero is a legitimate measurement here. Readiness must never wait until
    # the audio happens to pass; silence is judged by the following window.
    return 'ready'
}

function Assert-Gpvst3LevelChange {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Samples,
        [ValidateSet('track','global','device')][string]$Scope,
        [double]$MinRmsDbfs = -60,
        [double]$MinChangeDb = 3,
        [double]$MinRmsChange = 0.001,
        [int]$MinSamples = 6,
        [double]$MinSpanSeconds = 2,
        [int]$MaxGapMs = 750
    )
    if ($Samples.Count -lt $MinSamples) { throw "Insufficient fresh level measurements: $($Samples.Count) / $MinSamples." }
    $floor = [Math]::Pow(10, $MinRmsDbfs / 20)
    $previous = $null
    $silentSince = $null
    $active = 0
    $lastActive = 0.0
    $inputActive = 0
    $lastInputActive = 0.0
    $inputSilentSince = $null
    foreach ($sample in $Samples) {
        foreach ($name in @('sequence','sample_nanoseconds','observation_nanoseconds','audio_generation','selection_request_id',
                             'output_peak','output_rms','output_ac_rms')) {
            if ($null -eq $sample.$name -or -not ($sample.$name -is [ValueType]) -or
                [double]::IsNaN([double]$sample.$name) -or [double]::IsInfinity([double]$sample.$name) -or
                [double]$sample.$name -lt 0) { throw "Missing/invalid audio level value: $name" }
        }
        if (-not $sample.instance -or $sample.instance -eq '0' -or $sample.sequence -le 0 -or
            $sample.sample_nanoseconds -le 0 -or $sample.output_valid -ne $true) {
            throw 'Output level was not measured from a valid current buffer.'
        }
        $ageMs = ($sample.observation_nanoseconds - $sample.sample_nanoseconds) / 1000000.0
        if ($ageMs -lt 0 -or $ageMs -gt $MaxGapMs) { throw "Stale audio level measurement: ${ageMs} ms." }
        if ($sample.output_ac_rms -gt $sample.output_rms + 0.000001 -or
            $sample.output_rms -gt $sample.output_peak + 0.000001) { throw 'Inconsistent output peak/RMS/AC RMS.' }
        if ($previous) {
            if ($sample.instance -ne $previous.instance -or $sample.audio_generation -ne $previous.audio_generation -or
                $sample.selection_request_id -ne $previous.selection_request_id -or $sample.module -ne $previous.module -or
                $sample.class_id -ne $previous.class_id) { throw 'Plugin instance or selection changed during the level window.' }
            $gapMs = ($sample.sample_nanoseconds - $previous.sample_nanoseconds) / 1000000.0
            if ($sample.sequence -le $previous.sequence -or $gapMs -le 0 -or $gapMs -gt $MaxGapMs) {
                throw 'Repeated, regressing or interrupted audio level measurements.'
            }
        }
        $hasInput = $true
        if ($Scope -ne 'device') {
            if (-not $sample.module -or -not $sample.class_id) { throw 'VST3 module/class identity is missing from the level evidence.' }
            foreach ($name in @('input_peak','input_rms','input_ac_rms')) {
                if ($null -eq $sample.$name -or -not ($sample.$name -is [ValueType]) -or
                    [double]::IsNaN([double]$sample.$name) -or [double]::IsInfinity([double]$sample.$name) -or
                    [double]$sample.$name -lt 0) { throw "Missing/invalid VST3 input level: $name" }
            }
            if ($sample.input_valid -ne $true) { throw 'VST3 input level was not measured.' }
            if ($sample.input_ac_rms -gt $sample.input_rms + 0.000001 -or
                $sample.input_rms -gt $sample.input_peak + 0.000001) { throw 'Inconsistent VST3 input peak/RMS/AC RMS.' }
            $hasInput = $sample.input_ac_rms -ge $floor
            if ($hasInput) {
                ++$inputActive; $lastInputActive = $sample.sample_nanoseconds; $inputSilentSince = $null
            } else {
                if ($null -eq $inputSilentSince) { $inputSilentSince = $sample.sample_nanoseconds }
                if (($sample.sample_nanoseconds - $inputSilentSince) / 1000000.0 -ge $MaxGapMs) {
                    throw 'VST3 input stayed silent/DC-only; no usable score audio reached this instance.'
                }
            }
        }
        if ($sample.output_ac_rms -ge $floor -and $hasInput) {
            ++$active; $lastActive = $sample.sample_nanoseconds; $silentSince = $null
        } elseif ($hasInput) {
            if ($null -eq $silentSince) { $silentSince = $sample.sample_nanoseconds }
            if (($sample.sample_nanoseconds - $silentSince) / 1000000.0 -ge $MaxGapMs) {
                throw 'Output stayed silent/DC-only despite an active test signal.'
            }
        } else { $silentSince = $null }
        $previous = $sample
    }
    $span = ($Samples[-1].sample_nanoseconds - $Samples[0].sample_nanoseconds) / 1000000000.0
    if ($span -lt $MinSpanSeconds) { throw "Audio level window is too short: ${span} s." }
    if ($Scope -ne 'device' -and ($inputActive -lt 3 -or
        ($Samples[-1].sample_nanoseconds - $lastInputActive) / 1000000.0 -gt $MaxGapMs)) {
        throw 'VST3 input audio is missing through the end of the measurement window.'
    }
    if ($active -lt 3 -or ($Samples[-1].sample_nanoseconds - $lastActive) / 1000000.0 -gt $MaxGapMs) {
        throw 'No sustained AC output through the end of the measurement window.'
    }
    # Percentiles prevent a single impulse from passing as a clear level change.
    $rms = @($Samples | ForEach-Object { [double]$_.output_rms } | Sort-Object)
    $ac = @($Samples | ForEach-Object { [double]$_.output_ac_rms } | Sort-Object)
    $low = $rms[[int][Math]::Floor(($rms.Count - 1) * 0.2)]
    $high = $rms[[int][Math]::Ceiling(($rms.Count - 1) * 0.8)]
    $acLow = $ac[[int][Math]::Floor(($ac.Count - 1) * 0.2)]
    $acHigh = $ac[[int][Math]::Ceiling(($ac.Count - 1) * 0.8)]
    $changeDb = (ConvertTo-Gpvst3Dbfs $high) - (ConvertTo-Gpvst3Dbfs $low)
    $acChangeDb = (ConvertTo-Gpvst3Dbfs $acHigh) - (ConvertTo-Gpvst3Dbfs $acLow)
    $inputChangeDb = $null
    if ($Scope -ne 'device') {
        $inputRmsLevels = @($Samples | ForEach-Object { [double]$_.input_ac_rms } | Sort-Object)
        $inputLow = $inputRmsLevels[[int][Math]::Floor(($inputRmsLevels.Count - 1) * 0.2)]
        $inputHigh = $inputRmsLevels[[int][Math]::Ceiling(($inputRmsLevels.Count - 1) * 0.8)]
        $inputChangeDb = (ConvertTo-Gpvst3Dbfs $inputHigh) - (ConvertTo-Gpvst3Dbfs $inputLow)
        if ($inputHigh -lt $floor -or $inputHigh - $inputLow -lt $MinRmsChange -or $inputChangeDb -lt $MinChangeDb) {
            throw "VST3 $Scope input level did not change clearly: AC RMS ${inputChangeDb} dB; the input signal is missing or static."
        }
    }
    if ($high -lt $floor -or $acHigh -lt $floor -or $high - $low -lt $MinRmsChange -or
        $acHigh - $acLow -lt $MinRmsChange -or $changeDb -lt $MinChangeDb -or $acChangeDb -lt $MinChangeDb) {
        throw "No clear output level change: RMS ${changeDb} dB, AC RMS ${acChangeDb} dB; required $MinChangeDb dB and $MinRmsChange linear RMS."
    }
    [pscustomobject]@{passed=$true;scope=$Scope;module=$Samples[0].module;class_id=$Samples[0].class_id
        instance=$Samples[0].instance;samples=$Samples.Count;span_seconds=$span
        peak=($Samples.output_peak | Measure-Object -Maximum).Maximum
        rms_low=$low;rms_high=$high;rms_low_dbfs=(ConvertTo-Gpvst3Dbfs $low);rms_high_dbfs=(ConvertTo-Gpvst3Dbfs $high)
        change_db=$changeDb;ac_change_db=$acChangeDb;input_change_db=$inputChangeDb}
}

function Measure-Gpvst3AudioLevels {
    param(
        [Parameter(Mandatory)][string]$ObservationPath,
        [Parameter(Mandatory)][object[]]$Targets,
        [Parameter(Mandatory)][string]$EvidencePath,
        [ValidateRange(3,30)][double]$DurationSeconds = 4,
        [ValidateRange(1,300)][double]$ReadyTimeoutSeconds = 60,
        [ValidateRange(-120,0)][double]$MinRmsDbfs = -60,
        [ValidateRange(0.1,60)][double]$MinChangeDb = 3
    )
    $record = [ordered]@{schema=1;passed=$false;duration_seconds=$DurationSeconds
        ready_timeout_seconds=$ReadyTimeoutSeconds;ready_wait_seconds=0;ready_stage='preparing'
        thresholds=@{min_rms_dbfs=$MinRmsDbfs;min_change_db=$MinChangeDb;min_rms_change=0.001;max_gap_ms=750}
        targets=@();failure=$null}
    try {
        if (-not $Targets.Count) { throw 'No audio level targets were supplied.' }
        $readyWatch = [Diagnostics.Stopwatch]::StartNew()
        do {
            $baseline = Read-Gpvst3LevelObservation $ObservationPath
            if ($baseline.sample_mode -ne 'detailed') { throw 'Level acceptance requires GPVST3_DIAGNOSTIC_MODE=detailed before starting Guitar Pro.' }
            $record.ready_stage = Get-Gpvst3LevelReadiness $baseline $Targets
            $record.ready_wait_seconds = $readyWatch.Elapsed.TotalSeconds
            if ($record.ready_stage -eq 'ready') { break }
            if ($readyWatch.Elapsed.TotalSeconds -ge $ReadyTimeoutSeconds) {
                throw "Audio readiness timeout ($ReadyTimeoutSeconds s): $($record.ready_stage). The level window has not started."
            }
            Start-Sleep -Milliseconds 100
        } while ($true)
        $startNs = $baseline.gp_hook.observation_nanoseconds
        foreach ($target in $Targets) {
            $null = Get-Gpvst3LevelPoint $baseline $target
            $record.targets += [pscustomobject]@{scope=$target.scope;track_key=$target.track_key;module=$target.module
                samples=[Collections.Generic.List[object]]::new();summary=$null}
        }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds) {
            Start-Sleep -Milliseconds 100
            $observation = Read-Gpvst3LevelObservation $ObservationPath
            if ($observation.gp_hook.selection_status -in @('queued','preparing','failed')) {
                throw 'Selection left its prepared state during the level window.'
            }
            foreach ($target in $record.targets) {
                $point = Get-Gpvst3LevelPoint $observation $target
                if ($point.audio_generation -ne $baseline.gp_hook.audio_generation -or
                    $point.selection_request_id -ne $baseline.gp_hook.selection_request_id) {
                    throw 'Selection changed after the level window started.'
                }
                if ($point.sample_nanoseconds -le $startNs) { continue }
                $last = if ($target.samples.Count) { $target.samples[$target.samples.Count - 1] } else { $null }
                if ($last -and $last.instance -eq $point.instance -and $last.sequence -eq $point.sequence) { continue }
                $point | Add-Member -NotePropertyName received_elapsed_ms -NotePropertyValue $watch.Elapsed.TotalMilliseconds
                $target.samples.Add($point)
            }
        }
        foreach ($target in $record.targets) {
            try {
                $target.summary = Assert-Gpvst3LevelChange -Samples $target.samples.ToArray() -Scope $target.scope `
                    -MinRmsDbfs $MinRmsDbfs -MinChangeDb $MinChangeDb -MinSpanSeconds ($DurationSeconds - 1)
            } catch { throw "Audio level gate failed ($($target.scope) $($target.track_key)): $($_.Exception.Message)" }
            $latest = $target.samples[$target.samples.Count - 1]
            if (($observation.gp_hook.observation_nanoseconds - $latest.sample_nanoseconds) / 1000000.0 -gt 750 -or
                $watch.Elapsed.TotalMilliseconds - $latest.received_elapsed_ms -gt 750) {
                throw 'Audio levels stopped updating before the end of the window.'
            }
        }
        $record.passed = $true
        @($record.targets | ForEach-Object summary)
    } catch { $record.failure = $_.Exception.Message; throw }
    finally {
        $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $EvidencePath -Encoding UTF8
    }
}
