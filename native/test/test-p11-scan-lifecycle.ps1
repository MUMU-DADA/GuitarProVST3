param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p11-scan-lifecycle' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p7-catalog.ps1') -OutputRoot (Join-Path $OutputRoot 'catalog')
$e = Get-Content (Join-Path $OutputRoot 'catalog/verification.json') -Raw | ConvertFrom-Json
if (-not $e) { throw 'P11 scan evidence is missing.' }
@{complete=$true; automatic_scan_owner='bootstrap'; periodic_rescan=$false; coalescing='vst3_catalog.beginAsync'; evidence=$e} |
  ConvertTo-Json -Depth 30 | Set-Content (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P11 one-shot scan lifecycle and coalescing evidence. Evidence: $OutputRoot"
