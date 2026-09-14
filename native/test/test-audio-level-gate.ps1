param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'audio-level-check.ps1')
if (-not $OutputRoot) { $OutputRoot = Join-Path $PSScriptRoot '../../.tools/native/audio-level-gate' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

function New-LevelSeries {
    for ($i = 0; $i -lt 16; ++$i) {
        $rms = @(0.01,0.02,0.04,0.08)[$i % 4]
        [pscustomobject]@{
            instance='42';sequence=($i+1);sample_nanoseconds=[long](1000000000 + $i * 250000000)
            module='fixture.vst3';class_id='fixture-class'
            observation_nanoseconds=[long](1050000000 + $i * 250000000)
            audio_generation=1;selection_request_id=1
            input_valid=$true;input_peak=($rms*1.2);input_rms=($rms*0.875);input_ac_rms=($rms*0.875)
            output_valid=$true;output_peak=($rms * [Math]::Sqrt(2));output_rms=$rms;output_ac_rms=$rms
            # Deliberately large counters/true historical flags cannot rescue a bad signal.
            process_blocks=(100000+$i);write_observed=$true;vst3_output_non_silent=$true
        }
    }
}
$results = @()
$positive = Assert-Gpvst3LevelChange -Samples @(New-LevelSeries) -Scope global
if ([Math]::Abs($positive.change_db - 18.0617997) -gt 0.00001) { throw 'Known signal level delta was not measured correctly.' }
$results += @{case='varying_ac_signal';accepted=$true;change_db=$positive.change_db}
$inputPositive = Assert-Gpvst3LevelChange -Samples @(New-LevelSeries) -Scope track
$results += @{case='varying_track_input_and_output';accepted=$true;input_change_db=$inputPositive.input_change_db}
$cases = [ordered]@{
    all_zero = { param($s) foreach ($p in $s) { $p.output_peak=0; $p.output_rms=0; $p.output_ac_rms=0 } }
    first_good_then_zero = { param($s) foreach ($p in $s[1..15]) { $p.output_peak=0; $p.output_rms=0; $p.output_ac_rms=0 } }
    silent_at_end = { param($s) foreach ($p in $s[10..15]) { $p.output_peak=0; $p.output_rms=0; $p.output_ac_rms=0 } }
    varying_dc_only = { param($s) foreach ($p in $s) { $p.output_ac_rms=0 } }
    constant_level = { param($s) foreach ($p in $s) { $p.output_peak=0.1; $p.output_rms=0.07; $p.output_ac_rms=0.07 } }
    tiny_noise = { param($s) foreach ($p in $s) { $p.output_peak*=0.0001; $p.output_rms*=0.0001; $p.output_ac_rms*=0.0001 } }
    nan = { param($s) $s[4].output_rms=[double]::NaN }
    infinity = { param($s) $s[4].output_peak=[double]::PositiveInfinity }
    missing_level = { param($s) $s[4].output_rms=$null }
    stale_sample = { param($s) $s[4].observation_nanoseconds+=1000000000 }
    repeated_sequence = { param($s) foreach ($p in $s) { $p.sequence=1 } }
    replaced_instance = { param($s) $s[4].instance='99' }
    replaced_generation = { param($s) $s[4].audio_generation=2 }
    replaced_request = { param($s) $s[4].selection_request_id=2 }
    stopped_sampling = { param($s) $s[15].sample_nanoseconds+=2000000000; $s[15].observation_nanoseconds+=2000000000 }
    absent_input = { param($s) foreach ($p in $s) { $p.input_ac_rms=0 } }
    constant_input = { param($s) foreach ($p in $s) { $p.input_peak=0.1; $p.input_rms=0.07; $p.input_ac_rms=0.07 } }
    input_lost_at_end = { param($s) foreach ($p in $s[10..15]) { $p.input_peak=0; $p.input_rms=0; $p.input_ac_rms=0 } }
    input_dc_only = { param($s) foreach ($p in $s) { $p.input_ac_rms=0 } }
    input_nan = { param($s) $s[4].input_rms=[double]::NaN }
    missing_plugin_identity = { param($s) $s[4].module=$null }
}
foreach ($case in $cases.GetEnumerator()) {
    $samples = @(New-LevelSeries)
    & $case.Value $samples
    $rejection = $null
    try { $null = Assert-Gpvst3LevelChange -Samples $samples -Scope global }
    catch { $rejection = $_.Exception.Message }
    if (-not $rejection) { throw "Audio level gate falsely accepted: $($case.Key)" }
    $results += @{case=$case.Key;accepted=$false;reason=$rejection}
}

# A real timed file producer exercises the waiting/collection code, with a
# cold preparation phase and a ready-but-no-callback phase before audio.
$streamPath = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) 'cold-start-observation.json'
$point = @(New-LevelSeries)[0]
$stream = [pscustomobject]@{sample_mode='detailed';gp_hook=[pscustomobject]@{
    selection_status='preparing';selection_pending=$true;selection_request_id=1;audio_generation=1
    selection_committed_nanoseconds=0;runtime_processor_ready=$false;total_bypass=$false
    observation_nanoseconds=$point.observation_nanoseconds;vst3_output_level=$point
}}
$stream | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $streamPath -Encoding UTF8
$coldTarget = @(@{scope='global'})
if ((Get-Gpvst3LevelReadiness $stream $coldTarget) -ne 'preparing') { throw 'Cold start was accepted as ready.' }
$stream.gp_hook.selection_status='applied'; $stream.gp_hook.selection_pending=$false
$stream.gp_hook.runtime_processor_ready=$true; $stream.gp_hook.vst3_output_level.output_valid=$false
if ((Get-Gpvst3LevelReadiness $stream $coldTarget) -ne 'waiting_for_callback') { throw 'Readiness did not wait for the first measured callback.' }
$stream.gp_hook.vst3_output_level.output_valid=$true
$stream.gp_hook.vst3_output_level.output_rms=0; $stream.gp_hook.vst3_output_level.output_peak=0; $stream.gp_hook.vst3_output_level.output_ac_rms=0
if ((Get-Gpvst3LevelReadiness $stream $coldTarget) -ne 'ready') { throw 'Silence must enter level testing instead of waiting for a passing signal.' }
$producer = Start-Job -ArgumentList $streamPath,$stream -ScriptBlock {
    param($path,$state)
    $ErrorActionPreference = 'Stop'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $index = 0
    while ($clock.Elapsed.TotalSeconds -lt 9) {
        $elapsed = $clock.Elapsed.TotalSeconds
        $hook = $state.gp_hook
        $hook.selection_status = if ($elapsed -lt 2) {'preparing'} else {'applied'}
        $hook.selection_pending = $elapsed -lt 2
        $hook.runtime_processor_ready = $elapsed -ge 2
        $hook.selection_committed_nanoseconds = 3000000000
        $now = [long](1000000000 + $clock.Elapsed.TotalSeconds * 1000000000)
        $level = $hook.vst3_output_level
        $level.sample_nanoseconds=$now; $level.sequence=++$index
        $hook.observation_nanoseconds=$now + 1000000
        $level.output_valid=$elapsed -ge 3
        $rms = @(0.01,0.02,0.04,0.08)[$index % 4]
        $level.input_peak=$rms*1.2; $level.input_rms=$rms*0.875; $level.input_ac_rms=$rms*0.875
        $level.output_peak=$rms * [Math]::Sqrt(2); $level.output_rms=$rms; $level.output_ac_rms=$rms
        $json = $state | ConvertTo-Json -Depth 8
        $tmp = $path + '.tmp'
        [IO.File]::WriteAllText($tmp, $json)
        [IO.File]::Replace($tmp, $path, ($path + '.bak'))
        Start-Sleep -Milliseconds 100
    }
}
try {
    $timedPath = Join-Path $OutputRoot 'cold-start-levels.json'
    $null = Measure-Gpvst3AudioLevels -ObservationPath $streamPath -Targets $coldTarget -DurationSeconds 3 `
        -ReadyTimeoutSeconds 10 -EvidencePath $timedPath
    $timed = Get-Content -LiteralPath $timedPath -Raw | ConvertFrom-Json
    if (-not $timed.passed -or $timed.ready_wait_seconds -lt 3 -or $timed.targets[0].summary.span_seconds -lt 2) {
        throw 'Cold preparation time was counted as part of the level acceptance window.'
    }
    $results += @{case='cold_start_then_callback_then_level_window';accepted=$true;wait_seconds=$timed.ready_wait_seconds}
} finally {
    Stop-Job $producer
    Receive-Job $producer -ErrorAction Continue | Out-Host
    Remove-Job $producer -Force
}

$stream.gp_hook.selection_status='preparing'; $stream.gp_hook.selection_pending=$true
$stream | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $streamPath -Encoding UTF8
$timeoutPath = Join-Path $OutputRoot 'cold-start-timeout.json'
$rejected = $false
try {
    $null = Measure-Gpvst3AudioLevels -ObservationPath $streamPath -Targets $coldTarget -DurationSeconds 3 `
        -ReadyTimeoutSeconds 1 -EvidencePath $timeoutPath
} catch { $rejected = $_.Exception.Message -like '*readiness timeout*preparing*' }
if (-not $rejected) { throw 'Cold start timeout was not reported separately.' }
$results += @{case='cold_start_timeout';accepted=$false}
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: known 18.06 dB level change, cold start wait/timeout and $($cases.Count) rejected invalid audio cases. Evidence: $OutputRoot"
