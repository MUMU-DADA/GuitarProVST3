param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = '',
    [string]$PluginPath = '',
    [switch]$KeepHost,
    [switch]$SkipHost
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'test-p4-router.ps1')
if ($LASTEXITCODE) { throw 'P4 isolated router verification failed.' }

if (-not $SkipHost) {
    $runtime = Join-Path $PSScriptRoot 'test-p2-runtime.ps1'
    foreach ($route in @('input_insert', 'bus_mix')) {
        & $runtime -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost -EnableP4 -P4Route $route
        if ($LASTEXITCODE) { throw "P4 host status verification failed for route $route." }
    }
    & $runtime -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost -EnableP4 -P4Route bus_mix -ExpectP3TotalBypass
    if ($LASTEXITCODE) { throw 'P4 total bypass host verification failed.' }
}

Write-Output 'PASS: P4 capture router, input level status, input_insert/bus_mix routes and total bypass.'
