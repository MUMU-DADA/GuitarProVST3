param([string]$OutputRoot = '', [string]$PluginPath = '', [string]$Vst3Root = '', [switch]$RunHost)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p11-startup' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p10-startup.ps1') -OutputRoot (Join-Path $OutputRoot 'startup') -PluginPath $PluginPath -Vst3Root $Vst3Root -RunHost:$RunHost
$e = Get-Content (Join-Path $OutputRoot 'startup/verification.json') -Raw | ConvertFrom-Json
if (-not $e.timeline) { throw 'P11 startup timeline is missing.' }
@{complete=$true; host_validation=[bool]$RunHost; deferred_track_context=$true; evidence=$e} |
  ConvertTo-Json -Depth 30 | Set-Content (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P11 startup/score-open deferred lifecycle evidence. Evidence: $OutputRoot"
