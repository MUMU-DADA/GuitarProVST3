param([string]$DeviceMatrixPath = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p10-asio' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p10-baseline.ps1') -OutputRoot (Join-Path $OutputRoot 'fixture')
$evidence = Get-Content (Join-Path $OutputRoot 'fixture/verification.json') -Raw | ConvertFrom-Json
$matrix = @()
if ($DeviceMatrixPath) {
    if (-not (Test-Path -LiteralPath $DeviceMatrixPath -PathType Leaf)) { throw "ASIO device matrix not found: $DeviceMatrixPath" }
    $matrix = @(Get-Content -LiteralPath $DeviceMatrixPath -Raw | ConvertFrom-Json)
    foreach ($device in $matrix) {
        if ($device.sample_rate -lt 8000 -or $device.sample_rate -gt 192000 -or
            $device.block_size -le 0 -or $device.input_channels -lt 1 -or $device.output_channels -lt 1) {
            throw "Invalid ASIO matrix entry: $($device | ConvertTo-Json -Compress)"
        }
    }
}
@{mode='fixture_and_reported_components';device_matrix=$matrix;audio_deadline=$evidence.audio_deadline;
  roundtrip_latency=$evidence.roundtrip_latency;input_route=$evidence.input_route} |
    ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P10 ASIO deadline/configuration evidence (hardware loopback recorded separately). Evidence: $OutputRoot"
