param([string]$QtDir = '', [string]$OutputRoot = '', [switch]$RunHost)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p11-delivery-suite' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p11-startup.ps1') -OutputRoot (Join-Path $OutputRoot 'startup') -RunHost:$RunHost
& (Join-Path $PSScriptRoot 'test-p11-scan-lifecycle.ps1') -OutputRoot (Join-Path $OutputRoot 'scan-lifecycle')
& (Join-Path $PSScriptRoot 'test-p11-input-route.ps1') -OutputRoot (Join-Path $OutputRoot 'input-route')
@{complete=$true; host_validation=[bool]$RunHost; components=@('startup','scan-lifecycle','input-route')} |
  ConvertTo-Json -Depth 10 | Set-Content (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P11 startup, scan lifecycle and input route suite. Evidence: $OutputRoot"
