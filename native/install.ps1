param(
    [ValidateSet('Install', 'Uninstall', 'Status')]
    [string]$Action = 'Install',
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PackageDirectory = ''
)

$ErrorActionPreference = 'Stop'
if (-not $PackageDirectory) { $PackageDirectory = Split-Path -Parent $PSCommandPath }
$PackageDirectory = (Resolve-Path -LiteralPath $PackageDirectory).Path
$HostDirectory = [IO.Path]::GetFullPath($HostDirectory)
$sourceRelative = 'plugins/imageformats/guitarpro_vst3_autoload.dll'
$targetRelative = 'Plugins/imageformats/guitarpro_vst3_autoload.dll'
$targetPath = Join-Path $HostDirectory $targetRelative
$receiptPath = Join-Path $HostDirectory 'Plugins/guitarpro-vst3-install.json'

function Read-Receipt {
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) { return $null }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($receipt.schema -ne 1 -or $receipt.product -ne 'GuitarProVST3' -or
        $receipt.path -ne $targetRelative -or [string]$receipt.sha256 -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "Invalid GuitarProVST3 installation receipt: $receiptPath"
    }
    return $receipt
}

function Read-Package {
    $manifestPath = Join-Path $PackageDirectory 'package.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Package manifest not found: $manifestPath" }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $entry = @($manifest.files | Where-Object { $_.path -eq $sourceRelative })
    if ($entry.Count -ne 1 -or [string]$entry[0].sha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw 'Package manifest has no valid plugin entry.' }
    $source = Join-Path $PackageDirectory $sourceRelative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine [string]$entry[0].sha256) {
        throw 'Package plugin is missing or differs from package.json.'
    }
    [pscustomobject]@{manifest=$manifest;entry=$entry[0];source=$source}
}

function Assert-PlainDirectory([string]$path) {
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to use a non-ordinary directory: $path"
        }
    }
}

if ($Action -eq 'Status') {
    $receipt = Read-Receipt
    [pscustomobject]@{installed=($null -ne $receipt -and (Test-Path -LiteralPath $targetPath -PathType Leaf));target=$targetPath;receipt=$receiptPath}
    return
}

if (-not (Test-Path -LiteralPath (Join-Path $HostDirectory 'GuitarPro.exe') -PathType Leaf)) {
    throw "Guitar Pro executable not found: $HostDirectory"
}
foreach ($directory in @($HostDirectory, (Join-Path $HostDirectory 'Plugins'), (Split-Path -Parent $targetPath))) {
    Assert-PlainDirectory $directory
}
$running = @(Get-Process -Name GuitarPro -ErrorAction SilentlyContinue | Where-Object {
    try { [IO.Path]::GetFullPath($_.Path) -eq [IO.Path]::GetFullPath((Join-Path $HostDirectory 'GuitarPro.exe')) } catch { $false }
})
if ($running.Count) { throw 'Close Guitar Pro before installing or uninstalling GuitarProVST3.' }

$receipt = Read-Receipt
if ($Action -eq 'Install') {
    $package = Read-Package
    $expectedHash = [string]$package.entry.sha256
    if ($receipt) {
        if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ine [string]$receipt.sha256) {
            throw 'Owned plugin was modified or removed; refusing an in-place update.'
        }
        if ([string]$receipt.sha256 -ine $expectedHash) { throw 'Uninstall the existing version before installing a different package.' }
        [pscustomobject]@{installed=$true;target=$targetPath;sha256=$expectedHash;receipt=$receiptPath}
        return
    }
    if (Test-Path -LiteralPath $targetPath -PathType Leaf) {
        throw 'A plugin already exists without a GuitarProVST3 receipt; refusing to overwrite it.'
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $targetPath) | Out-Null
    $created = $false
    try {
        Copy-Item -LiteralPath $package.source -Destination $targetPath
        $created = $true
        $actual = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
        if ($actual -ine $expectedHash) { throw 'Installed plugin hash differs from the package.' }
        [ordered]@{schema=1;product='GuitarProVST3';version=[string]$package.manifest.version;
                   path=$targetRelative;sha256=$actual;installed_at=[DateTime]::UtcNow.ToString('o')} |
            ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
        [pscustomobject]@{installed=$true;target=$targetPath;sha256=$actual;receipt=$receiptPath}
    } catch {
        if ($created -and (Test-Path -LiteralPath $targetPath)) { Remove-Item -LiteralPath $targetPath -Force }
        throw
    }
    return
}

if (-not $receipt) { throw 'No GuitarProVST3 installation receipt found; refusing to remove an unowned plugin.' }
if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) { throw 'Owned plugin is missing; refusing to remove the receipt.' }
if ((Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ine [string]$receipt.sha256) {
    throw 'Owned plugin was modified; refusing to remove it.'
}
Remove-Item -LiteralPath $targetPath -Force
Remove-Item -LiteralPath $receiptPath -Force
[pscustomobject]@{installed=$false;target=$targetPath;receipt=$receiptPath}
