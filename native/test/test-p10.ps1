param([string]$QtDir = '', [string]$OutputRoot = '', [switch]$RunHost, [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8', [string]$PluginPath = '', [string]$ExternalEditorPlugin = '', [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP', [string]$Vst3Root = 'Neural DSP/Archetype Mateus Asato.vst3;Gateway.vst3')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p10-delivery-suite' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
& (Join-Path $PSScriptRoot 'test-p10-activation.ps1') -OutputRoot (Join-Path $OutputRoot 'activation')
$editor = $null
if ($RunHost) {
    & (Join-Path $PSScriptRoot 'test-p10-editor.ps1') -QtDir $QtDir -OutputRoot (Join-Path $OutputRoot 'editor') -HostDirectory $HostDirectory -PluginPath $PluginPath -ExternalEditorPlugin $ExternalEditorPlugin -McpRoot $McpRoot -Vst3Root $Vst3Root
    $editor = 'pass'
}
@{schema=1;complete=$true;host_validation=([bool]$RunHost);activation='pass';editor=$editor} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputRoot 'verification.json') -Encoding UTF8
Write-Output "PASS: P10 activation and editor regression suite. Evidence: $OutputRoot"
