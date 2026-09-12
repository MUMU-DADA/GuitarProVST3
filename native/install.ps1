param(
    [ValidateSet('Install', 'Update', 'Uninstall', 'Status')]
    [string]$Action = 'Install',
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PackageDirectory = '',
    [switch]$Elevate,
    [switch]$MigrateExisting,
    [Parameter(DontShow = $true)]
    [string]$ErrorLogPath = ''
)

$ErrorActionPreference = 'Stop'
trap {
    $record = $_
    if ($ErrorLogPath) {
        try { $record | Out-String -Width 4096 | Set-Content -LiteralPath $ErrorLogPath -Encoding UTF8 } catch { }
    }
    throw $record
}
if (-not $PackageDirectory) { $PackageDirectory = Split-Path -Parent $PSCommandPath }
$PackageDirectory = (Resolve-Path -LiteralPath $PackageDirectory).Path
$HostDirectory = [IO.Path]::GetFullPath($HostDirectory)
$dataScript = $PSCommandPath
if ($Elevate -and $Action -in @('Install','Update','Uninstall')) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $errorLog = Join-Path ([IO.Path]::GetTempPath()) ('GuitarProVST3-install-' + [guid]::NewGuid().ToString('N') + '.log')
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $dataScript + '"'),'-Action',$Action,
            '-HostDirectory',('"' + $HostDirectory + '"'),'-PackageDirectory',('"' + $PackageDirectory + '"'),
            '-ErrorLogPath',('"' + $errorLog + '"'))
        if ($MigrateExisting) { $arguments += '-MigrateExisting' }
        try {
            $child = Start-Process -FilePath powershell.exe -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
            if ($child.ExitCode) {
                $details = if (Test-Path -LiteralPath $errorLog) { (Get-Content -LiteralPath $errorLog -Raw -Encoding UTF8).Trim() } else { '' }
                if ($details) { throw $details }
                throw "Elevated installer failed (exit $($child.ExitCode)). Run install.ps1 from an administrator terminal for details."
            }
        } finally {
            if (Test-Path -LiteralPath $errorLog) { Remove-Item -LiteralPath $errorLog -Force }
        }
        Write-Output "$Action completed."
        return
    }
}
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
if ($Action -in @('Install','Update')) {
    $package = Read-Package
    $expectedHash = [string]$package.entry.sha256
    $legacyBackup = $null
    if ($receipt) {
        if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ine [string]$receipt.sha256) {
            throw 'Owned plugin was modified or removed; refusing an in-place update.'
        }
        if ([string]$receipt.sha256 -ieq $expectedHash) {
            [pscustomobject]@{installed=$true;target=$targetPath;sha256=$expectedHash;receipt=$receiptPath;updated=$false}
            return
        }
        $temporary = "$targetPath.$([guid]::NewGuid().ToString('N')).tmp"
        $backup = "$temporary.backup"
        try {
            Copy-Item -LiteralPath $package.source -Destination $temporary
            if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ine $expectedHash) { throw 'Package hash changed while staging update.' }
            [IO.File]::Replace($temporary, $targetPath, $backup)
            [ordered]@{schema=1;product='GuitarProVST3';version=[string]$package.manifest.version;
                       path=$targetRelative;sha256=$expectedHash;installed_at=[DateTime]::UtcNow.ToString('o')} |
                ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
            if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }
            [pscustomobject]@{installed=$true;target=$targetPath;sha256=$expectedHash;receipt=$receiptPath;updated=$true}
        } catch {
            if (Test-Path -LiteralPath $backup) { [IO.File]::Replace($backup, $targetPath, [NullString]::Value) }
            throw
        } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
        return
    }
    if (Test-Path -LiteralPath $targetPath -PathType Leaf) {
        if (-not $MigrateExisting) {
            throw 'A plugin already exists without a GuitarProVST3 receipt; refusing to overwrite it. Run Install.cmd -MigrateExisting to back it up and install this package.'
        }
        $backupDirectory = Join-Path $HostDirectory 'Plugins/guitarpro-vst3-backups'
        Assert-PlainDirectory $backupDirectory
        New-Item -ItemType Directory -Force -Path $backupDirectory | Out-Null
        Assert-PlainDirectory $backupDirectory
        $backupName = 'guitarpro_vst3_autoload-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N') + '.dll.bak'
        $legacyBackup = Join-Path $backupDirectory $backupName
        Move-Item -LiteralPath $targetPath -Destination $legacyBackup
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $targetPath) | Out-Null
    $created = $false
    try {
        Copy-Item -LiteralPath $package.source -Destination $targetPath
        $created = $true
        $actual = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
        if ($actual -ine $expectedHash) { throw 'Installed plugin hash differs from the package.' }
        $newReceipt = [ordered]@{schema=1;product='GuitarProVST3';version=[string]$package.manifest.version;
                   path=$targetRelative;sha256=$actual;installed_at=[DateTime]::UtcNow.ToString('o')}
        if ($legacyBackup) {
            $newReceipt['previous_unowned_backup'] = $legacyBackup.Substring($HostDirectory.Length + 1).Replace('\','/')
        }
        $newReceipt |
            ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
        [pscustomobject]@{installed=$true;target=$targetPath;sha256=$actual;receipt=$receiptPath;updated=($null -ne $legacyBackup);backup=$legacyBackup}
    } catch {
        if ($created -and (Test-Path -LiteralPath $targetPath)) { Remove-Item -LiteralPath $targetPath -Force }
        if ($legacyBackup -and (Test-Path -LiteralPath $legacyBackup)) { Move-Item -LiteralPath $legacyBackup -Destination $targetPath }
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
