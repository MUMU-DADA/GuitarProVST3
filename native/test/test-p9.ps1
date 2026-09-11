param([string]$QtDir = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '../.tools/native/p9-delivery-suite' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p9-switch.ps1') -OutputRoot (Join-Path $OutputRoot 'switch')
& (Join-Path $PSScriptRoot 'test-p9-ui.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'ui')
Write-Output "PASS: P9 switch and UI regression suite. Evidence: $OutputRoot"
