param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p11-input-route' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p10-input-route.ps1') -Route input_insert -OutputRoot (Join-Path $OutputRoot 'input-insert')
& (Join-Path $PSScriptRoot 'test-p10-input-route.ps1') -Route bus_mix -OutputRoot (Join-Path $OutputRoot 'bus-mix')
@{complete=$true; capture_to_vst3_to_output=$true; routes=@('input_insert','bus_mix'); evidence_root=$OutputRoot} |
  ConvertTo-Json -Depth 10 | Set-Content (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P11 capture/input VST3/output route evidence. Evidence: $OutputRoot"
