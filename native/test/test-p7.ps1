param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
foreach ($path in @((Join-Path $HostDirectory 'GuitarPro.exe'), $PluginPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "P7 prerequisite not found: $path" }
}

function Get-PeArchitecture([string]$path) {
    $stream = [IO.File]::OpenRead($path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset + 4
        $machine = $reader.ReadUInt16()
        if ($machine -eq 0x8664) { return 'x64' }
        if ($machine -eq 0x014c) { return 'x86' }
        return ('0x{0:X4}' -f $machine)
    } finally { $reader.Dispose(); $stream.Dispose() }
}

$run = Join-Path $root ('artifacts/p7-' + [guid]::NewGuid().ToString('N'))
$hostCopy = Join-Path $root ('.tools/p7-host-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run, $hostCopy | Out-Null
Get-ChildItem -LiteralPath $HostDirectory -File | Where-Object { $_.Extension -in '.dll','.conf' -or $_.Name -eq 'GuitarPro.exe' } | Copy-Item -Destination $hostCopy
Copy-Item -LiteralPath (Join-Path $HostDirectory 'Plugins') -Destination $hostCopy -Recurse
$imageDir = Join-Path $hostCopy 'Plugins/imageformats'
New-Item -ItemType Directory -Force -Path $imageDir | Out-Null
Copy-Item -LiteralPath $PluginPath -Destination (Join-Path $imageDir 'guitarpro_vst3_autoload.dll')

$names = @('GuitarPro.exe','GPCore.dll','GPRSE.dll','AMAudio.dll','AMOverloud.dll')
$hostFiles = [ordered]@{}
foreach ($name in $names) {
    $path = Join-Path $HostDirectory $name
    if (-not (Test-Path -LiteralPath $path)) { throw "Locked host file missing: $path" }
    $item = Get-Item -LiteralPath $path
    $hostFiles[$name] = [ordered]@{
        version = $item.VersionInfo.FileVersion
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        pe_architecture = Get-PeArchitecture $path
    }
}

$saved = @{}
foreach ($name in @('GPVST3_DATA_DIR','GPVST3_VST3_PATHS','GPVST3_VST3_ROOT','GPVST3_ENABLE_P2_HOOK','GPVST3_ENABLE_P2_EFFECT','TEMP','TMP')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$process = $null
try {
    Remove-Item Env:GPVST3_VST3_PATHS,Env:GPVST3_VST3_ROOT,Env:GPVST3_ENABLE_P2_HOOK,Env:GPVST3_ENABLE_P2_EFFECT -ErrorAction SilentlyContinue
    $env:GPVST3_DATA_DIR = $run
    $env:TEMP = $run
    $env:TMP = $run
    $process = Start-Process -FilePath (Join-Path $hostCopy 'GuitarPro.exe') -WorkingDirectory $hostCopy -WindowStyle Hidden -PassThru
    $statusPath = Join-Path $run 'status.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (-not (Test-Path -LiteralPath $statusPath) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if ($process.HasExited) { throw "Guitar Pro exited before P7 status publication ($($process.ExitCode))." }
    }
    if (-not (Test-Path -LiteralPath $statusPath)) { throw 'P7 status was not published.' }
    $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    $catalog = @($status.vst3_catalog | Where-Object { $_.compatible })
    if ($status.qt_ui -ne 'panel_ready_p7') { throw "P7 UI state missing: $($status.qt_ui)" }
    if ($catalog.Count -eq 0) { throw 'P7 automatic VST3 catalog is empty.' }
    if ([int]$status.vst3_host.modules_discovered -lt 1) { throw 'P7 standard-directory scan found no VST3 bundle.' }
    $evidence = [ordered]@{
        complete = $false
        host_files = $hostFiles
        host_directory = (Resolve-Path -LiteralPath $HostDirectory).Path
        scan = [ordered]@{
            modules_discovered = $status.vst3_host.modules_discovered
            modules_loaded = $status.vst3_host.modules_loaded
            classes_enumerated = $status.vst3_host.classes_enumerated
            compatible_catalog_count = $catalog.Count
            names = @($catalog | ForEach-Object { $_.name } | Sort-Object -Unique)
            mode = 'metadata_only_scan'
        }
        same_level_entry = 'host_limited: current ABI evidence exposes soundsContainer but no stable parent insertion contract'
        realtime_selection = 'not_implemented: P7 UI state is persisted; list-driven processor publication remains host_limited'
        native_editor = 'host_limited: IPlugView/HWND bridge is not verified for Guitar Pro 8.1.1.17'
        state = 'enabled=true and legacy bypass is migrated by sidecar layer'
        plugin_sha256 = (Get-FileHash -LiteralPath $PluginPath -Algorithm SHA256).Hash
    }
    $evidence | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P7 automatic catalog, host lock baseline and explicit host-limited boundaries. Evidence: $run"
} finally {
    if ($process -and -not $process.HasExited) {
        if (-not $KeepHost) { try { $process.CloseMainWindow() | Out-Null } catch { } }
        if (-not $KeepHost -and -not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    foreach ($name in $saved.Keys) {
        $value = $saved[$name]
        if ($null -eq $value) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { Set-Item "Env:$name" $value }
    }
}
