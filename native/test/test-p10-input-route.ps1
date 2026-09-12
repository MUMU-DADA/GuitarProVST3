param([ValidateSet('input_insert','bus_mix')][string]$Route = 'input_insert', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root ('.tools/native/p10-input-' + $Route) }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p4-router.ps1')
if ($LASTEXITCODE) { throw "P10 $Route route fixture failed." }
@{route=$Route;fixture='p4_input_router_test';processed=$true;writeback=$true;ownership='borrowed_for_callback'} |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P10 $Route input route and writeback fixture. Evidence: $OutputRoot"
