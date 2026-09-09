param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not (Test-Path -LiteralPath $PluginPath)) { throw 'Build the plugin first with native/build.ps1.' }
if (-not (Test-Path -LiteralPath (Join-Path $HostDirectory 'GuitarPro.exe'))) { throw "Host not found: $HostDirectory" }

$run = Join-Path $root ('artifacts/p0-' + [guid]::NewGuid().ToString('N'))
$hostCopy = Join-Path $root ('.tools/autoload-host-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run,$hostCopy | Out-Null
Get-ChildItem -LiteralPath $HostDirectory -File | Where-Object { $_.Extension -in '.dll','.conf' -or $_.Name -eq 'GuitarPro.exe' } | Copy-Item -Destination $hostCopy
Copy-Item -LiteralPath (Join-Path $HostDirectory 'Plugins') -Destination $hostCopy -Recurse
if (Test-Path -LiteralPath (Join-Path $HostDirectory 'translations')) { Copy-Item -LiteralPath (Join-Path $HostDirectory 'translations') -Destination $hostCopy -Recurse }
$imageDir = Join-Path $hostCopy 'Plugins/imageformats'
Remove-Item -LiteralPath (Join-Path $imageDir 'guitarpro_mcp_autoload.dll') -Force -ErrorAction SilentlyContinue
$installedPlugin = Join-Path $imageDir 'guitarpro_vst3_autoload.dll'
Copy-Item -LiteralPath $PluginPath -Destination $installedPlugin
$statusPath = Join-Path $run 'status.json'
$exe = Join-Path $hostCopy 'GuitarPro.exe'
$shortcutPath = Join-Path $run 'Guitar Pro P0.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $exe
$shortcut.WorkingDirectory = $hostCopy
$shortcut.Save()

$saved = @{}
foreach ($name in @('QT_PLUGIN_PATH','QT_QPA_GENERIC_PLUGINS','GPVST3_DATA_DIR','TEMP','TMP')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$results = @()
try {
    Remove-Item Env:QT_PLUGIN_PATH,Env:QT_QPA_GENERIC_PLUGINS -ErrorAction SilentlyContinue
    $env:GPVST3_DATA_DIR = $run
    $env:TEMP = $run
    $env:TMP = $run
    foreach ($variant in @('direct','shortcut')) {
        Remove-Item -LiteralPath $statusPath -Force -ErrorAction SilentlyContinue
        $launch = @{FilePath=$exe; WorkingDirectory=$hostCopy; WindowStyle='Hidden'; PassThru=$true}
        if ($variant -eq 'shortcut') { $launch.FilePath = $shortcutPath }
        $process = Start-Process @launch
        try {
            $deadline = [DateTime]::UtcNow.AddSeconds(20)
            do {
                Start-Sleep -Milliseconds 200
                $process.Refresh()
            } while (-not (Test-Path -LiteralPath $statusPath) -and -not $process.HasExited -and [DateTime]::UtcNow -lt $deadline)
            if (-not (Test-Path -LiteralPath $statusPath)) { throw "P0 automatic loading failed for $variant." }
            $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
            if (-not $status.loaded -or -not $status.bypassed -or -not $status.host_supported) { throw "Unexpected P0 status for $variant." }
            $results += [pscustomobject]@{variant=$variant;pid=$process.Id;status=$status.status;host_supported=$status.host_supported;bypassed=$status.bypassed}
        } finally {
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force; $process.WaitForExit(5000) | Out-Null }
            $process.Dispose()
        }
    }
    Remove-Item -LiteralPath $installedPlugin -Force
    Remove-Item -LiteralPath $statusPath -Force -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $exe -WorkingDirectory $hostCopy -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 3
    if (Test-Path -LiteralPath $statusPath) { throw 'Removing the plugin did not restore normal startup.' }
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force; $process.WaitForExit(5000) | Out-Null }
    $results += [pscustomobject]@{variant='uninstalled';pid=$process.Id;status='not_loaded';host_supported=$false;bypassed=$true}
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    [pscustomobject]@{host_directory=$hostCopy;plugin_sha256=(Get-FileHash -LiteralPath $PluginPath).Hash;results=$results} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'verification.json')
    if (-not $KeepHost) { Remove-Item -LiteralPath $hostCopy -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Output "PASS: P0 automatic load, default bypass and uninstall recovery. Evidence: $run"
