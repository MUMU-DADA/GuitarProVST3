param([string]$QtDir = '', [string]$OutputRoot = '', [string]$PluginPath = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '../.tools/native/p8-regression' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p8-state.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'state')
& (Join-Path $PSScriptRoot 'test-p8-recognition.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'recognition')
& (Join-Path $PSScriptRoot 'test-p8-recognition-timeout.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'recognition-timeout')
& (Join-Path $PSScriptRoot 'test-p8-ui.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'ui')
& (Join-Path $PSScriptRoot 'test-p8-runtime.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'runtime') -PluginPath $PluginPath
Write-Output "PASS: P8 state, recognition, UI and runtime regression suite."
