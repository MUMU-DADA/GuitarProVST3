param(
    [Parameter(Mandatory)][string]$ObservationPath,
    [ValidateSet('track','global','device')][string]$Scope = 'track',
    [string]$TrackKey = '',
    [string]$ExpectedModule = '',
    [double]$DurationSeconds = 4,
    [double]$ReadyTimeoutSeconds = 60,
    [double]$MinRmsDbfs = -60,
    [double]$MinChangeDb = 3,
    [string]$EvidencePath = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'audio-level-check.ps1')
$ObservationPath = (Resolve-Path -LiteralPath $ObservationPath).Path
if (-not $EvidencePath) {
    $EvidencePath = Join-Path (Split-Path -Parent $ObservationPath) ('audio-levels-' + [guid]::NewGuid().ToString('N') + '.json')
}
if ($Scope -eq 'track' -and -not $TrackKey) { throw 'Specify the exact -TrackKey from track_runtime_evidence.' }
$result = Measure-Gpvst3AudioLevels -ObservationPath $ObservationPath -Targets @(@{scope=$Scope;track_key=$TrackKey;module=$ExpectedModule}) `
    -EvidencePath $EvidencePath -DurationSeconds $DurationSeconds -ReadyTimeoutSeconds $ReadyTimeoutSeconds `
    -MinRmsDbfs $MinRmsDbfs -MinChangeDb $MinChangeDb
$result | Format-Table scope,input_change_db,rms_low_dbfs,rms_high_dbfs,change_db,ac_change_db,peak -AutoSize
Write-Output "PASS: measured output level change. Evidence: $EvidencePath"
