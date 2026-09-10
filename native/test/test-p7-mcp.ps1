param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = '',
    [string]$Vst3Root = 'ParametricOD.vst3;Gateway.vst3',
    [ValidateSet('enabled', 'default', 'disabled')]
    [string]$HookMode = 'enabled',
    [switch]$StandardScan,
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
$mcpGeneric = Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll'
$mcpClient = Join-Path $McpRoot 'native/mcp-client.ps1'
foreach ($path in @((Join-Path $HostDirectory 'GuitarPro.exe'), $PluginPath, $mcpGeneric, $mcpClient)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "P7 MCP prerequisite not found: $path" }
}

$run = Join-Path $root ('artifacts/mcp-p7-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
. (Join-Path $PSScriptRoot 'host-session.ps1')
$before = Get-Gpvst3HostSnapshot $HostDirectory
$dataDirectory = Join-Path $run 'data'
$fixtureSource = Join-Path $McpRoot 'native/testdata/minimal.gp'
if (-not (Test-Path -LiteralPath $fixtureSource)) { $fixtureSource = Join-Path $McpRoot 'test/testdata/minimal.gp' }
if (-not (Test-Path -LiteralPath $fixtureSource)) { throw "P7 MCP fixture not found under $McpRoot." }
$fixture = Join-Path $run 'p7-runtime.gp'
Copy-Item -LiteralPath $fixtureSource -Destination $fixture

$process = $null
$session = $null
try {
    $environment = @{GPVST3_DATA_DIR=$dataDirectory}
    if ($HookMode -ne 'default') { $environment.GPVST3_ENABLE_P2_HOOK = if ($HookMode -eq 'disabled') { '0' } else { '1' } }
    # P7 owns its selected processors; leave the legacy single-effect probe
    # disabled so this test exercises the list-driven lifecycle in isolation.
    $environment.GPVST3_ENABLE_P2_EFFECT = '0'
    $programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    $vst3Paths = @($Vst3Root -split ';' | Where-Object { $_ } |
        ForEach-Object { Join-Path $programFiles ('Common Files/VST3/' + $_) })
    if (-not $StandardScan) {
        $environment.GPVST3_RUNTIME_VST3 = $vst3Paths[0]
        $environment.GPVST3_VST3_ROOT = $vst3Paths -join ';'
    }
    $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $run -McpRoot $McpRoot -Environment $environment
    $statusPath = Join-Path $run 'data/status.json'
    $sessionPath = Join-Path $run 'mcp/native-session.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) -and
           [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if ($process.HasExited) { throw "Guitar Pro exited before MCP/P7 startup ($($process.ExitCode))." }
    }
    if (-not (Test-Path -LiteralPath $statusPath) -or -not (Test-Path -LiteralPath $sessionPath)) {
        throw 'MCP/P7 startup files were not published.'
    }
    $identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
    $descriptor = Get-Content -LiteralPath $sessionPath -Raw | ConvertFrom-Json
    if ($descriptor.pid -ne $process.Id) { throw 'MCP session belongs to another process.' }
    . $mcpClient
    $session = New-McpSession -SessionFile $sessionPath
    $opened = Invoke-McpTool $session gp_open @{path=$fixture}
    $operation = $null
    $openDeadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $operation = Invoke-McpTool $session gp_operation @{request=$opened.request}
    } while ($operation.operation.status -notin @('opened','error','cancelled') -and [DateTime]::UtcNow -lt $openDeadline)
    if ($operation.operation.status -ne 'opened') { throw "P7 MCP fixture did not open: $($operation | ConvertTo-Json -Depth 12 -Compress)" }
    Invoke-McpTool $session gp_activate @{document=$operation.operation.document} | Out-Null
    $scanDeadline = [DateTime]::UtcNow.AddSeconds(90)
    do {
        $scan = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        if (-not $scan.vst3_host.scan_pending -and $scan.vst3_host.status -ne 'scanning') { break }
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) { throw 'Guitar Pro exited during P7 catalog scan.' }
    } while ([DateTime]::UtcNow -lt $scanDeadline)
    if ($scan.vst3_host.scan_pending -or $scan.vst3_host.status -eq 'scanning') {
        throw 'P7 MCP catalog scan did not complete within 90 seconds.'
    }
    if ($HookMode -ne 'enabled' -and ($scan.gp_hook.installed -or -not $scan.gp_hook.total_bypass)) {
        throw 'Default/disabled startup must remain unpatched and bypassed before a selection.'
    }
    Start-Sleep -Milliseconds 500
    $sound = Invoke-McpTool $session gp_objects @{query='soundsContainer';limit=30}
    $panel = Invoke-McpTool $session gp_objects @{query='gpvst3P7Panel';limit=30}
    $entry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=30}
    $result = [ordered]@{
        identity = $identity
        hook_mode = $HookMode
        standard_scan = [bool]$StandardScan
        startup = $scan
        sound = $sound
        entry = $entry
        panel_before = $panel
    }
    if (@($entry.objects).Count -eq 0) { throw "P7 sound-section entry was not found: $($entry | ConvertTo-Json -Depth 20 -Compress)" }
    $entryObject = @($entry.objects)[0]
    $trigger = Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=$entryObject.id}
    # The sidebar can rebuild after opening a score. Wait for the 500 ms
    # attachment timer to finish instead of racing it at exactly one tick.
    $panelDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 100
        $panelAfter = Invoke-McpTool $session gp_objects @{query='gpvst3P7Panel';limit=30}
        $panelObject = @($panelAfter.objects | Where-Object object_name -EQ 'gpvst3P7Panel') | Select-Object -First 1
    } while ((!$panelObject -or $panelObject.parent_name -ne 'soundsContainer') -and [DateTime]::UtcNow -lt $panelDeadline)
    $result.panel_after = $panelAfter
    if (-not $panelObject) { throw 'P7 panel was not found after opening the sound-section entry.' }
    if ($panelObject.parent_name -ne 'soundsContainer') {
        throw "P7 panel is not a direct child of soundsContainer: $($panelObject | ConvertTo-Json -Depth 12 -Compress)"
    }
    $result.trigger = $trigger
    $checkboxes = Invoke-McpTool $session gp_objects @{query='gpvst3Enabled';limit=30}
    if (@($checkboxes.objects).Count -lt 2) { throw "P7 catalog did not expose two effects: $($checkboxes | ConvertTo-Json -Depth 12 -Compress)" }
    $firstClass = @($scan.vst3_catalog | Where-Object { $_.module -eq $vst3Paths[0] -and $_.compatible })[0]
    $secondClass = @($scan.vst3_catalog | Where-Object { $_.module -eq $vst3Paths[1] -and $_.compatible })[0]
    $checkbox = @($checkboxes.objects | Where-Object object_name -EQ ("gpvst3Enabled_" + $firstClass.class_id))[0]
    $secondCheckbox = @($checkboxes.objects | Where-Object object_name -EQ ("gpvst3Enabled_" + $secondClass.class_id))[0]
    if (-not $checkbox -or -not $secondCheckbox) {
        throw "Requested regression effects are missing from the catalog. Scan errors: $($scan.vst3_host.errors -join '; ')"
    }
    $result.checkbox_before = @($checkbox,$secondCheckbox)
    $setFirst = Invoke-McpTool $session gp_set_property @{
        snapshot=$checkboxes.snapshot
        id=$checkbox.id
        property='checked'
        value=$true
    }
    if ($HookMode -eq 'disabled') {
        $after = Invoke-McpTool $session gp_objects @{query=$checkbox.object_name;limit=10}
        $notice = Invoke-McpTool $session gp_objects @{query='gpvst3Status';limit=10}
        $result.disabled_selection = $after
        $result.disabled_notice = $notice
        $noticeJson = $notice | ConvertTo-Json -Depth 12 -Compress
        if ($noticeJson -notmatch 'realtime_disabled_by_environment') {
            throw "Disabled hook failure did not explain the actual cause: $noticeJson"
        }
        $persisted = Get-Content -LiteralPath (Join-Path $dataDirectory 'effect-chain.json') -Raw | ConvertFrom-Json
        if (@($persisted.effects | Where-Object enabled).Count) { throw 'Rejected selection was saved as enabled.' }
        $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
        Write-Output "PASS: P7 explicitly disabled hook rejects selection with its actual cause. Evidence: $run"
        return
    }
    Start-Sleep -Seconds 2
    $play = Invoke-McpTool $session gp_playback @{operation='play';document=$operation.operation.document}
    Start-Sleep -Seconds 2
    $observationPath = Join-Path $dataDirectory 'p2-observation.json'
    if (-not (Test-Path -LiteralPath $observationPath)) { throw 'P7 realtime observation was not written.' }
    $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
    $hook = $observation.gp_hook
    $result.one_enabled_observation = $hook
    if (-not $hook.runtime_processor_ready -or $hook.runtime_effect_instances -ne 1 -or
        $hook.chain_active_slot -lt 0 -or $hook.total_bypass -or -not $hook.runtime_process_observed) {
        throw "P7 checked selection did not produce one live processor: $($hook | ConvertTo-Json -Depth 12 -Compress)"
    }
    $checkboxes2 = Invoke-McpTool $session gp_objects @{query='gpvst3Enabled';limit=30}
    $second2 = @($checkboxes2.objects | Where-Object object_name -EQ $secondCheckbox.object_name)[0]
    $setSecond = Invoke-McpTool $session gp_set_property @{
        snapshot=$checkboxes2.snapshot
        id=$second2.id
        property='checked'
        value=$true
    }
    Start-Sleep -Seconds 3
    $twoObservation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
    $result.two_enabled_observation = $twoObservation.gp_hook
    if (-not $twoObservation.gp_hook.runtime_processor_ready -or
        $twoObservation.gp_hook.runtime_effect_instances -ne 2 -or
        $twoObservation.gp_hook.chain_active_slot -lt 0 -or $twoObservation.gp_hook.total_bypass) {
        throw "P7 two-item selection did not produce a live serial chain: $($twoObservation.gp_hook | ConvertTo-Json -Depth 12 -Compress)"
    }
    $classId = ([string]$checkbox.object_name) -replace '^gpvst3Enabled_', ''
    $editorButtons = Invoke-McpTool $session gp_objects @{query=("gpvst3Editor_$classId");limit=10}
    if (@($editorButtons.objects).Count -eq 0) { throw 'P7 native editor button was not found.' }
    $editorButton = @($editorButtons.objects)[0]
    $result.editor_button = $editorButton
    $editorTrigger = Invoke-McpTool $session gp_trigger @{snapshot=$editorButtons.snapshot;id=$editorButton.id}
    Start-Sleep -Seconds 1
    $editorHost = Invoke-McpTool $session gp_objects @{query='gpvst3NativeEditorHost';limit=10}
    $result.editor_host = $editorHost
    $result.editor_trigger = $editorTrigger
    $editorHostObject = @($editorHost.objects | Where-Object object_name -EQ 'gpvst3NativeEditorHost')[0]
    if (-not $editorHostObject -or $editorHostObject.parent_name -ne 'gpvst3P7Panel') {
        throw "P7 native editor host is not a child of the P7 panel: $($editorHost | ConvertTo-Json -Depth 12 -Compress)"
    }
    $result.editor_observation = (Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json).gp_hook
    if ($result.editor_observation.runtime_effect_error) {
        throw "P7 native editor reported a runtime error: $($result.editor_observation.runtime_effect_error)"
    }
    $result.editor_status = Invoke-McpTool $session gp_objects @{query='gpvst3Status';limit=10} -AllowError
    if (@($result.editor_status.objects | Where-Object { $_.properties.text -eq ('原生 GUI 已打开：' + $firstClass.name) }).Count -eq 0) {
        throw "P7 native editor status was not published: $($result.editor_status.objects.properties.text -join '; ')"
    }
    $editorButtons2 = Invoke-McpTool $session gp_objects @{query=("gpvst3Editor_$classId");limit=10}
    $editorButton2 = @($editorButtons2.objects)[0]
    $editorReopen = Invoke-McpTool $session gp_trigger @{snapshot=$editorButtons2.snapshot;id=$editorButton2.id}
    Start-Sleep -Milliseconds 500
    $result.editor_reopen = Invoke-McpTool $session gp_objects @{query='gpvst3NativeEditorHost';limit=10}
    if (@($result.editor_reopen.objects | Where-Object object_name -EQ 'gpvst3NativeEditorHost').Count -eq 0) {
        throw 'P7 native editor did not remain available after a repeated open request.'
    }
    $uncheckedQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3Enabled_$classId");limit=10}
    $uncheckedObject = @($uncheckedQuery.objects)[0]
    $setFirstOff = Invoke-McpTool $session gp_set_property @{
        snapshot=$uncheckedQuery.snapshot
        id=$uncheckedObject.id
        property='checked'
        value=$false
    }
    Start-Sleep -Seconds 2
    $oneRemaining = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
    $result.one_remaining_observation = $oneRemaining.gp_hook
    if ($oneRemaining.gp_hook.total_bypass -or $oneRemaining.gp_hook.runtime_effect_instances -ne 1 -or
        $oneRemaining.gp_hook.chain_active_slot -lt 0) {
        throw "P7 disabling one item did not keep the remaining processor active: $($oneRemaining.gp_hook | ConvertTo-Json -Depth 12 -Compress)"
    }
    $secondOffQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3Enabled_$(([string]$secondCheckbox.object_name) -replace '^gpvst3Enabled_', '')");limit=10}
    $secondOffObject = @($secondOffQuery.objects)[0]
    $setSecondOff = Invoke-McpTool $session gp_set_property @{
        snapshot=$secondOffQuery.snapshot
        id=$secondOffObject.id
        property='checked'
        value=$false
    }
    Start-Sleep -Seconds 2
    Invoke-McpTool $session gp_playback @{operation='stop';document=$operation.operation.document} | Out-Null
    $disabledObservation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
    $result.disabled_observation = $disabledObservation.gp_hook
    if (-not $disabledObservation.gp_hook.total_bypass -or $disabledObservation.gp_hook.runtime_effect_instances -ne 0 -or
        $disabledObservation.gp_hook.chain_active_slot -ne -1) {
        throw "P7 disabling all items did not return to direct bypass: $($disabledObservation.gp_hook | ConvertTo-Json -Depth 12 -Compress)"
    }
    $sidecarPath = Join-Path $dataDirectory 'effect-chain.json'
    if (-not (Test-Path -LiteralPath $sidecarPath)) { throw 'P7 sidecar was not written.' }
    $sidecar = Get-Content -LiteralPath $sidecarPath -Raw | ConvertFrom-Json
    $savedStates = @($sidecar.effects | Where-Object { $_.component_state })
    if ($savedStates.Count -lt 2) {
        throw "P7 sidecar did not retain component state for both selected plugins: $($sidecar | ConvertTo-Json -Depth 8 -Compress)"
    }
    $result.sidecar = $sidecar
    $restoreQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3Enabled_$classId");limit=10}
    $restoreObject = @($restoreQuery.objects)[0]
    Invoke-McpTool $session gp_set_property @{
        snapshot=$restoreQuery.snapshot
        id=$restoreObject.id
        property='checked'
        value=$true
    } | Out-Null
    Start-Sleep -Seconds 2
    $restoredObservation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
    $result.state_restore_observation = $restoredObservation.gp_hook
    if (-not $restoredObservation.gp_hook.runtime_processor_ready -or
        $restoredObservation.gp_hook.runtime_effect_instances -ne 1 -or
        $restoredObservation.gp_hook.chain_active_slot -lt 0 -or
        $restoredObservation.gp_hook.runtime_effect_error) {
        $notice = Invoke-McpTool $session gp_objects @{query='gpvst3Status';limit=10}
        $result.restore_notice = $notice
        $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
        throw "P7 sidecar state did not restore a live processor: $($notice.objects.properties.toolTip -join '; '). Evidence: $run"
    }
    $restoreOffQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3Enabled_$classId");limit=10}
    $restoreOffObject = @($restoreOffQuery.objects)[0]
    Invoke-McpTool $session gp_set_property @{
        snapshot=$restoreOffQuery.snapshot
        id=$restoreOffObject.id
        property='checked'
        value=$false
    } | Out-Null
    Start-Sleep -Seconds 1
    $result.set_first = $setFirst
    $result.set_second = $setSecond
    $result.set_first_off = $setFirstOff
    $result.set_second_off = $setSecondOff
    $result.playback = $play
    $result.status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P7 MCP same-level panel, catalog and realtime selection lifecycle. Evidence: $run"
}
finally {
    if ($session) { try { Close-McpSession $session } catch {} }
    try { Stop-Gpvst3TestHost $process -KeepHost:$KeepHost -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
