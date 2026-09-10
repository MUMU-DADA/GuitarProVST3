param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$PluginPath = '',
    [switch]$KeepHost,
    [switch]$RequireP1,
    [switch]$RequireP2,
    [switch]$RequireP2Hook,
    [ValidateSet('', 'GuitarPro.exe', 'GPCore.dll', 'GPRSE.dll', 'AMAudio.dll', 'AMOverloud.dll')]
    [string]$RejectHostFile = '',
    [string]$QtDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not (Test-Path -LiteralPath $PluginPath)) { throw 'Build the plugin first with native/build.ps1.' }
if (-not (Test-Path -LiteralPath (Join-Path $HostDirectory 'GuitarPro.exe'))) { throw "Host not found: $HostDirectory" }

$run = Join-Path $root ('artifacts/p0-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
. (Join-Path $PSScriptRoot 'host-session.ps1')
$before = Get-Gpvst3HostSnapshot $HostDirectory
if ($RejectHostFile) {
    $negativeBuild = Join-Path $run 'rejected host plugin'
    # Keep the compiler environment out of the parent regression process.
    $buildArguments = @('-NoProfile', '-File', (Join-Path $root 'native/build.ps1'),
                        '-OutputRoot', $negativeBuild, '-RejectHostFile', $RejectHostFile)
    if ($QtDir) { $buildArguments += @('-QtDir', $QtDir) }
    & (Get-Process -Id $PID).Path @buildArguments | Out-Host
    if ($LASTEXITCODE) { throw 'Negative host-gate plugin build failed.' }
    $PluginPath = Join-Path $negativeBuild 'plugins/imageformats/guitarpro_vst3_autoload.dll'
}
$results = @()
$passed = $false
$environment = @{}
if ($RequireP2Hook -or $RejectHostFile) { $environment.GPVST3_ENABLE_P2_HOOK = '1' }
if ($RejectHostFile) {
    $environment.GPVST3_ENABLE_P2_EFFECT = '1'
    $environment.GPVST3_ENABLE_P4_INPUT = '1'
    $environment.GPVST3_P4_ROUTE = 'bus_mix'
}
if ($RequireP1 -or $RequireP2) {
    $programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    $environment.GPVST3_VST3_ROOT = (@('Gateway.vst3','ParametricOD.vst3','NAM Rig.vst3') |
        ForEach-Object { Join-Path $programFiles ('Common Files/VST3/' + $_) }) -join ';'
}
try {
    foreach ($variant in @('direct','shortcut')) {
        $variantRun = Join-Path $run $variant
        $statusPath = Join-Path $variantRun 'status.json'
        $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $variantRun -Environment $environment -Shortcut:($variant -eq 'shortcut')
        try {
            # P7 scans all standard VST3 bundles once on first launch; large
            # installations can take longer than the P0 three-plugin probe.
            $deadline = [DateTime]::UtcNow.AddSeconds(90)
            do {
                Start-Sleep -Milliseconds 200
                $process.Refresh()
            } while (-not (Test-Path -LiteralPath $statusPath) -and -not $process.HasExited -and [DateTime]::UtcNow -lt $deadline)
            if (-not (Test-Path -LiteralPath $statusPath)) { throw "P0 development loading failed for $variant." }
            $identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath
            $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
            if ($RequireP1 -or $RequireP2) {
                while ($status.vst3_host.scan_pending -and [DateTime]::UtcNow -lt $deadline) {
                    Start-Sleep -Milliseconds 200
                    $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
                }
                if ($status.vst3_host.scan_pending) { throw 'VST3 lifecycle scan did not finish.' }
            }
            if (-not $status.loaded -or -not $status.bypassed -or $process.HasExited -or $status.pid -ne $process.Id) { throw "Unexpected P0 status for $variant." }
            if ($RejectHostFile) {
                foreach ($file in $status.host_files.PSObject.Properties) {
                    if ($file.Value -ne ($file.Name -ne $RejectHostFile)) { throw "Unexpected host hash result: $($file.Name)" }
                }
                if ($status.host_supported -or $status.status -ne 'host_unsupported' -or
                    $status.gp_hook.installed -or $status.gp_hook.enabled -or $status.gp_hook.runtime_processor_ready -or
                    $status.gp_hook.runtime_effect_enabled -or $status.gp_hook.input_route_enabled -or
                    $status.vst3_host.ready -or -not $status.gp_hook.total_bypass) {
                    throw "Host hash gate did not disable realtime mode for $variant."
                }
            } elseif (-not $status.host_supported) { throw "Unexpected unsupported host status for $variant." }
            if ($RequireP1) {
                if (-not $status.vst3_host -or -not $status.vst3_host.worker_thread -or -not $status.vst3_host.ready) {
                    throw "Unexpected P1 VST3 host status for $variant."
                }
            }
            if ($RequireP2) {
                if (-not $status.audio_adapter -or $status.audio_adapter.status -ne 'planar_float32' -or
                    -not $status.audio_adapter.scratch_prepared_off_thread) {
                    throw "Unexpected P2 audio adapter status for $variant."
                }
                if (-not $status.vst3_host -or $status.vst3_host.process_probes_passed -lt 1) {
                    throw "VST3 process probe did not pass for $variant."
                }
                if (-not $status.gp_hook) { throw "Missing P2 GP hook status for $variant." }
                if ($RequireP2Hook) {
                    if (-not $status.gp_hook.enabled -or -not $status.gp_hook.installed -or
                        -not $status.gp_hook.observation_only) {
                        throw "Runtime P2 GP observation hook did not install for $variant."
                    }
                } elseif (-not $status.gp_hook.observation_only -or $status.gp_hook.installed) {
                    throw "Unexpected P2 GP hook status for $variant."
                }
            }
            $observationPath = Join-Path $variantRun 'p2-observation.json'
            $observation = $null
            if ($RequireP2Hook) {
                $observationDeadline = [DateTime]::UtcNow.AddSeconds(3)
                while (-not (Test-Path -LiteralPath $observationPath) -and [DateTime]::UtcNow -lt $observationDeadline) { Start-Sleep -Milliseconds 50 }
                if (-not (Test-Path -LiteralPath $observationPath)) { throw "P2 observation snapshot was not written for $variant." }
                $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
            }
            $results += [pscustomobject]@{variant=$variant;identity=$identity;pid=$process.Id;status=$status.status;host_supported=$status.host_supported;bypassed=$status.bypassed;vst3_host=$status.vst3_host;audio_adapter=$status.audio_adapter;gp_hook=$status.gp_hook;p2_observation=$observation}
        } finally {
            Stop-Gpvst3TestHost $process -RunDirectory $variantRun
        }
    }
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -RunDirectory (Join-Path $run 'without-development') -WithoutDevelopmentPlugin
    try {
        Start-Sleep -Seconds 3
        $process.Refresh()
        if ($process.HasExited) { throw 'Guitar Pro exited without the development loading environment.' }
        $loaded = @($process.Modules | Where-Object ModuleName -EQ 'guitarpro_vst3_autoload.dll' | ForEach-Object FileName)
        if ($loaded -icontains (Resolve-Path -LiteralPath $PluginPath).Path) { throw 'Development DLL is still loaded without its environment.' }
        $installed = Join-Path $HostDirectory 'Plugins/imageformats/guitarpro_vst3_autoload.dll'
        if (Test-Path -LiteralPath $installed) {
            if ($loaded -inotcontains (Resolve-Path -LiteralPath $installed).Path) { throw 'Existing installed plugin did not resume normal loading.' }
        } elseif ($loaded.Count) { throw 'An unexpected VST3 bootstrap loaded without the development environment.' }
        $results += [pscustomobject]@{variant='without_development_environment';pid=$process.Id;executable=$process.Path;loaded_plugins=$loaded;development_plugin_loaded=$false}
        $passed = $true
    } finally { Stop-Gpvst3TestHost $process -KeepHost:$KeepHost -RunDirectory (Join-Path $run 'without-development') }
} finally {
    Assert-Gpvst3HostUnchanged $before $HostDirectory $run
    [pscustomobject]@{passed=$passed;mode='original_host_no_install';host_directory=(Resolve-Path -LiteralPath $HostDirectory).Path;rejected_host_file=$RejectHostFile;plugin_sha256=(Get-FileHash -LiteralPath $PluginPath).Hash;results=$results} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'verification.json')
}
if ($RejectHostFile) {
    Write-Output "PASS: P0 host hash mismatch disabled hook and realtime mode. Evidence: $run"
} elseif ($RequireP2) {
    Write-Output "PASS: P0/P1 plus P2 planar adapter, process probe and GP observation status. Evidence: $run"
} elseif ($RequireP1) {
    Write-Output "PASS: P0 development load plus P1 VST3 host lifecycle. Evidence: $run"
} else {
    Write-Output "PASS: P0 original-host development load, default bypass and environment removal. Evidence: $run"
}
