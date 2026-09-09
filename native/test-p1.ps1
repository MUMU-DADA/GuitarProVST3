param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'
$p0 = Join-Path $PSScriptRoot 'test-p0.ps1'
& $p0 -HostDirectory $HostDirectory -PluginPath $PluginPath -KeepHost:$KeepHost -RequireP1
if ($LASTEXITCODE) { throw 'P1 verification failed.' }
