param([string]$QtDir = '', [string]$OutputRoot = '', [switch]$RunHost)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '../.tools/native/p10-delivery-suite' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p10-baseline.ps1') -OutputRoot (Join-Path $OutputRoot 'baseline')
& (Join-Path $PSScriptRoot 'test-p10-startup.ps1') -OutputRoot (Join-Path $OutputRoot 'startup') -RunHost:$RunHost
& (Join-Path $PSScriptRoot 'test-p10-asio.ps1') -OutputRoot (Join-Path $OutputRoot 'asio')
& (Join-Path $PSScriptRoot 'test-p10-input-route.ps1') -Route input_insert -OutputRoot (Join-Path $OutputRoot 'input-insert')
& (Join-Path $PSScriptRoot 'test-p10-input-route.ps1') -Route bus_mix -OutputRoot (Join-Path $OutputRoot 'bus-mix')
& (Join-Path $PSScriptRoot 'test-p10-ui.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'ui')
@{complete=$true;host_validation=[bool]$RunHost;components=@('baseline','startup','asio','input_insert','bus_mix','ui')} |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P10 baseline, startup, ASIO, input route and UI suite. Evidence: $OutputRoot"
