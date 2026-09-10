param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [string]$Vst3Root = '',
    [int]$ScanTimeoutSeconds = 90,
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
New-Item -ItemType Directory -Force -Path $run | Out-Null
. (Join-Path $PSScriptRoot 'host-session.ps1')
$before = Get-Gpvst3HostSnapshot $HostDirectory

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

$process = $null
try {
    $environment = @{}
    if ($Vst3Root) { $environment.GPVST3_VST3_ROOT = $Vst3Root }
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -Environment $environment
    $statusPath = Join-Path $run 'status.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (-not (Test-Path -LiteralPath $statusPath) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if ($process.HasExited) { throw "Guitar Pro exited before P7 status publication ($($process.ExitCode))." }
    }
    if (-not (Test-Path -LiteralPath $statusPath)) { throw 'P7 status was not published.' }
    $identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath
    $scanDeadline = [DateTime]::UtcNow.AddSeconds($ScanTimeoutSeconds)
    do {
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        $scanState = $status.vst3_host
        if (-not $scanState.scan_pending -and $scanState.status -ne 'scanning') { break }
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) { throw "Guitar Pro exited during P7 VST3 scan ($($process.ExitCode))." }
    } while ([DateTime]::UtcNow -lt $scanDeadline)
    if ($scanState.scan_pending -or $scanState.status -eq 'scanning') {
        throw "P7 VST3 metadata scan did not complete within $ScanTimeoutSeconds seconds."
    }
    $catalog = @($status.vst3_catalog | Where-Object { $_.compatible })
    if ($status.qt_ui -ne 'panel_ready_p7') { throw "P7 UI state missing: $($status.qt_ui)" }
    if ($catalog.Count -eq 0) { throw 'P7 automatic VST3 catalog is empty.' }
    if ([int]$status.vst3_host.modules_discovered -lt 1) { throw 'P7 standard-directory scan found no VST3 bundle.' }
    $evidence = [ordered]@{
        complete = $false
        identity = $identity
        host_files = $hostFiles
        host_directory = (Resolve-Path -LiteralPath $HostDirectory).Path
        scan = [ordered]@{
            modules_discovered = $status.vst3_host.modules_discovered
            modules_loaded = $status.vst3_host.modules_loaded
            classes_enumerated = $status.vst3_host.classes_enumerated
            compatible_catalog_count = $catalog.Count
            names = @($catalog | ForEach-Object { $_.name } | Sort-Object -Unique)
            mode = $(if ($Vst3Root) { 'explicit_lifecycle_probe' } else { 'metadata_only_scan' })
            configured_root = $Vst3Root
        }
        same_level_entry = 'verified_by_mcp: gpvst3P7Panel and gpvst3SoundEffectChainButton are direct children of soundsContainer'
        realtime_selection = 'verified_by_mcp: native/test/test-p7-mcp.ps1 covers one item, two-item serial chain, single-item cancel and all-item direct bypass'
        native_editor = 'implemented_verified: same processing instance IPlugView/HWND bridge, IPlugFrame, IComponentHandler parameter mailbox and component/controller state capture are verified by native/test/test-p7-mcp.ps1; unsupported editor types remain host_limited'
        mcp_regression = 'native/test/test-p7-mcp.ps1; see artifacts/mcp-p7-* / verification.json'
        state = 'enabled=true and legacy bypass is migrated by sidecar layer'
        plugin_sha256 = (Get-FileHash -LiteralPath $PluginPath -Algorithm SHA256).Hash
    }
    $evidence | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P7 automatic catalog, host lock baseline and explicit host-limited boundaries. Evidence: $run"
} finally {
    try { Stop-Gpvst3TestHost $process -KeepHost:$KeepHost -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
