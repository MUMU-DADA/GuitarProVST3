param([string]$QtDir = '', [string]$OutputRoot = '', [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8', [string]$PluginPath = '', [string]$ExternalEditorPlugin = '', [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP', [string]$Vst3Root = 'Neural DSP/Archetype Mateus Asato.vst3;Gateway.vst3')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputRoot) { $OutputRoot = Join-Path $root '.tools/native/p10-editor-test' }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
# The fixture runs production RuntimeEffect in the real Guitar Pro Qt process;
# the MCP flow then drives the product's selector/editor commands and captures
# the visible native window. No computer-use automation is involved.
$runtimeOutput = @(& (Join-Path $PSScriptRoot 'test-p8-runtime.ps1') -QtDir $QtDir -OutputRoot $OutputRoot -HostDirectory $HostDirectory -PluginPath $PluginPath -ExternalEditorPlugin $ExternalEditorPlugin)
$runtimeOutput | Write-Output
$runtimeLine = $runtimeOutput | Where-Object { $_ -match 'Evidence: (.+)$' } | Select-Object -Last 1
if (-not $runtimeLine -or $runtimeLine -notmatch 'Evidence: (.+)$') { throw 'P10 editor fixture did not report its evidence directory.' }
$runtimeDirectory = $Matches[1].Trim()
$record = Get-Content -LiteralPath (Join-Path $runtimeDirectory 'runtime-test.json') -Raw | ConvertFrom-Json
if ($record.result -ne 0) { throw 'P10 editor lifecycle fixture failed.' }
$mcpOutput = @(& (Join-Path $PSScriptRoot 'test-p7-mcp.ps1') -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -Vst3Root $Vst3Root -EditorOnly)
$mcpOutput | Write-Output
$mcpLine = $mcpOutput | Where-Object { $_ -match 'Evidence: (.+)$' } | Select-Object -Last 1
if (-not $mcpLine -or $mcpLine -notmatch 'Evidence: (.+)$') { throw 'P10 MCP editor flow did not report its evidence directory.' }
$mcpDirectory = $Matches[1].Trim()
$mcp = Get-Content -LiteralPath (Join-Path $mcpDirectory 'verification.json') -Raw | ConvertFrom-Json
if (-not $mcp.editor_window.visible -or $mcp.editor_observation.editor_stage -ne 'visible' -or
    $mcp.after_editor_close.editor_stage -ne 'removed' -or -not $mcp.editor_reopen.visible) {
    throw 'P10 MCP editor/reopen/close evidence is incomplete.'
}
@{result='pass';editor_lifecycle='qt_native_hwnd';external_editor=([bool]$ExternalEditorPlugin);runtime=$runtimeDirectory;mcp=$mcpDirectory} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputRoot 'p10-editor.json') -Encoding UTF8
Write-Output "PASS: P10 editor lifecycle, MCP product flow and native HWND capture. Evidence: $OutputRoot"
