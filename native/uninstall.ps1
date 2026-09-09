param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PackageDirectory = ''
)

$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot 'install.ps1'
& $installer -Action Uninstall -HostDirectory $HostDirectory -PackageDirectory $PackageDirectory
if ($LASTEXITCODE) { throw 'GuitarProVST3 uninstall failed.' }
