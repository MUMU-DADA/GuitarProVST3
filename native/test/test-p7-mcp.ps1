param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = 'C:\Users\mumu\source\GuitarProMCP',
    [string]$PluginPath = '',
    [string]$Vst3Root = 'ParametricOD.vst3;Gateway.vst3',
    [ValidateSet('enabled', 'default', 'disabled')]
    [string]$HookMode = 'enabled',
    [switch]$StandardScan,
    [switch]$CheckGain,
    [switch]$CheckCatalogRestart,
    [switch]$KeepHost
)

$ErrorActionPreference = 'Stop'

function Set-P7SoundSection($Session, $Restore = $null) {
    Invoke-McpTool $Session gp_window @{state='restore'} | Out-Null
    $previous = @{}
    foreach ($name in @('tabScoreButton')) {
        $query = Invoke-McpTool $Session gp_objects @{query=$name;limit=10}
        $target = @($query.objects | Where-Object object_name -EQ $name)[0]
        if (-not $target) { throw "Sound-section control was not found: $name" }
        $previous[$name] = [bool]$target.properties.checked
        $desired = if ($Restore) { $Restore[$name] } else { $true }
        if ($previous[$name] -ne $desired) {
            if (-not $target.enabled) { throw "Sound-section control unavailable: $name; checked=$($previous[$name]); desired=$desired" }
            Invoke-McpTool $Session gp_trigger @{snapshot=$query.snapshot;id=$target.id} | Out-Null
            Start-Sleep -Milliseconds 100
        }
    }
    return $previous
}

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
. (Join-Path $PSScriptRoot 'p7-window-capture.ps1')
$before = Get-Gpvst3HostSnapshot $HostDirectory
$dataDirectory = Join-Path $run 'data'
$fixtureSource = Join-Path $McpRoot 'native/testdata/minimal.gp'
if (-not (Test-Path -LiteralPath $fixtureSource)) { $fixtureSource = Join-Path $McpRoot 'test/testdata/minimal.gp' }
if (-not (Test-Path -LiteralPath $fixtureSource)) { throw "P7 MCP fixture not found under $McpRoot." }
$fixture = Join-Path $run 'p7-runtime.gp'
Copy-Item -LiteralPath $fixtureSource -Destination $fixture
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($fixture, [IO.Compression.ZipArchiveMode]::Update)
try {
    $zipEntry = $archive.GetEntry('Content/score.gpif')
    $reader = [IO.StreamReader]::new($zipEntry.Open())
    try { [xml]$gpif = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $gpif.GPIF.Tracks.Track.AudioEngineState = 'RSE'
    $zipEntry.Delete()
    $writer = [IO.StreamWriter]::new($archive.CreateEntry('Content/score.gpif').Open(), [Text.UTF8Encoding]::new($false))
    try { $writer.Write($gpif.OuterXml) } finally { $writer.Dispose() }
} finally { $archive.Dispose() }

$process = $null
$session = $null
$soundSectionBefore = $null
try {
    $environment = @{GPVST3_DATA_DIR=$dataDirectory}
    if ($HookMode -ne 'default') { $environment.GPVST3_ENABLE_P2_HOOK = if ($HookMode -eq 'disabled') { '0' } else { '1' } }
    # P7 owns its selected processors; leave the legacy single-effect probe
    # disabled so this test exercises the list-driven lifecycle in isolation.
    $environment.GPVST3_ENABLE_P2_EFFECT = '0'
    $programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    $vst3Paths = @($Vst3Root -split ';' | Where-Object { $_ } |
        ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $programFiles ('Common Files/VST3/' + $_) } })
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
    # Use a real RSE template with audible notes; changing AudioEngineState
    # alone does not create a native instrument sound for every minimal file.
    $created = Invoke-McpTool $session gp_new @{template='Steel Guitar'}
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $operation = Invoke-McpTool $session gp_operation @{request=$created.request}
    } while ($operation.operation.status -notin @('created','error','cancelled') -and [DateTime]::UtcNow -lt $deadline)
    if ($operation.operation.status -ne 'created') { throw 'P7 RSE template did not create a document.' }
    $riff = Invoke-McpTool $session gp_insert_tab @{document=$operation.operation.document;track=0;string=0;bar=0;text='0-2-5-7';mode='replace';denominator=4}
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do { Start-Sleep -Milliseconds 100; $riffState = Invoke-McpTool $session gp_operation @{request=$riff.request} }
    while ($riffState.operation.status -notin @('applied','error','cancelled') -and [DateTime]::UtcNow -lt $deadline)
    if ($riffState.operation.status -ne 'applied') { throw 'P7 audible riff was not applied.' }
    $fixture = Join-Path $run 'p7-playback.gp'
    $save = Invoke-McpTool $session gp_save_as @{document=$operation.operation.document;path=$fixture}
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do { Start-Sleep -Milliseconds 100; $saveState = Invoke-McpTool $session gp_operation @{request=$save.request} }
    while ($saveState.operation.status -notin @('saved','error','cancelled') -and [DateTime]::UtcNow -lt $deadline)
    if ($saveState.operation.status -ne 'saved') { throw 'P7 playback fixture was not saved.' }
    Invoke-McpTool $session gp_activate @{document=$operation.operation.document} | Out-Null
    $scanDeadline = [DateTime]::UtcNow.AddSeconds(90)
    do {
        $scan = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        if (-not $scan.vst3_host.scan_pending -and -not $scan.vst3_host.recognition_pending -and $scan.vst3_host.status -ne 'scanning' -and
            -not @($scan.vst3_catalog | Where-Object { $_.recognition_status -in @('queued','running') }).Count) { break }
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
    $soundSectionBefore = Set-P7SoundSection $session
    $sound = Invoke-McpTool $session gp_objects @{query='soundsContainer';limit=30}
    $panel = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalPanel';limit=30}
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
    $panelClock = [Diagnostics.Stopwatch]::StartNew()
    $trigger = Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=$entryObject.id}
    # The sidebar can rebuild after opening a score. Wait for the 500 ms
    # attachment timer to finish instead of racing it at exactly one tick.
    $panelDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 100
        $panelAfter = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalPanel';limit=30}
        $panelObject = @($panelAfter.objects | Where-Object object_name -EQ 'gpvst3GlobalPanel') | Select-Object -First 1
    } while ((!$panelObject -or $panelObject.parent_name -ne 'gpvst3GlobalVst3Section') -and [DateTime]::UtcNow -lt $panelDeadline)
    $result.panel_after = $panelAfter
    $result.cold_list_open_ms = $panelClock.ElapsedMilliseconds
    if (-not $panelObject) { throw 'P7 panel was not found after opening the sound-section entry.' }
    if ($panelObject.parent_name -ne 'gpvst3GlobalVst3Section') {
        throw "P7 panel is not mounted inside the global section: $($panelObject | ConvertTo-Json -Depth 12 -Compress)"
    }
    $anchor = (Invoke-McpTool $session gp_objects @{query='soundMastering';limit=10}).objects | Where-Object object_name -eq 'soundMastering'
    $section = (Invoke-McpTool $session gp_objects @{query='gpvst3GlobalVst3Section';limit=10}).objects | Where-Object object_name -eq 'gpvst3GlobalVst3Section'
    if (-not $panelObject.visible -or -not $anchor.visible -or -not $section.visible -or
        $section.parent_name -ne $anchor.parent_name -or $section.properties.y -lt ($anchor.properties.y + $anchor.properties.height)) {
        throw 'Global content is not visible after the native mastering controls.'
    }
    $result.trigger = $trigger
    if ($scan.vst3_host.modules_loaded -ne 0 -or $scan.vst3_host.instances_created -ne 0) {
        throw 'Static scan counters unexpectedly contain module loads or processor creation.'
    }
    $requested = @()
    foreach ($modulePath in $vst3Paths) {
        $identifyDeadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            $scan = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
            $candidate = @($scan.vst3_catalog | Where-Object { [IO.Path]::GetFullPath($_.module) -ieq [IO.Path]::GetFullPath($modulePath) -and $_.class_id -and $_.recognition_status -eq 'ready' })[0]
            if ($candidate) { break }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $identifyDeadline)
        if (-not $candidate) { throw "Background identification did not publish a ready audio class: $modulePath" }
        $requested += $candidate
    }
    if ($requested.Count -ge 2) {
        $firstClass = $requested[0]
        $secondClass = $requested[1]
        $checkboxes = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalEnabled';limit=100}
        $checkbox = @($checkboxes.objects | Where-Object object_name -EQ ("gpvst3GlobalEnabled_" + $firstClass.class_id))[0]
        $secondCheckbox = @($checkboxes.objects | Where-Object object_name -EQ ("gpvst3GlobalEnabled_" + $secondClass.class_id))[0]
        if (-not $checkbox -or -not $secondCheckbox) { throw 'Identified checkbox not found.' }
    } else { throw 'P7 regression requires two distinct ready test plugins.' }
    $result.checkbox_before = @($checkbox,$secondCheckbox)
    $freshFirst = Invoke-McpTool $session gp_objects @{query=$checkbox.object_name;limit=10}
    $setFirst = Invoke-McpTool $session gp_set_property @{
        snapshot=$freshFirst.snapshot
        id=@($freshFirst.objects)[0].id
        property='checked'
        value=$true
    }
    if ($HookMode -eq 'disabled') {
        $after = Invoke-McpTool $session gp_objects @{query=$checkbox.object_name;limit=10}
        $notice = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalStatus';limit=10}
        $result.disabled_selection = $after
        $result.disabled_notice = $notice
        $noticeJson = $notice | ConvertTo-Json -Depth 12 -Compress
        if ($noticeJson -notmatch 'realtime_disabled_by_environment') {
            throw "Disabled hook failure did not explain the actual cause: $noticeJson"
        }
        $persisted = Get-Content -LiteralPath (Join-Path $dataDirectory 'effect-chain.json') -Raw | ConvertFrom-Json
        if (@($persisted.global.effects | Where-Object enabled).Count) { throw 'Rejected selection was saved as enabled.' }
        $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
        Write-Output "PASS: P7 explicitly disabled hook rejects selection with its actual cause. Evidence: $run"
        return
    }
    Start-Sleep -Seconds 2
    $initialPlayback = Invoke-McpTool $session gp_playback @{operation='state';document=$operation.operation.document}
    Invoke-McpTool $session gp_playback @{operation='set_loop';document=$operation.operation.document;enabled=$true} | Out-Null
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
    $checkboxes2 = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalEnabled';limit=30}
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
    $classId = ([string]$checkbox.object_name) -replace '^gpvst3GlobalEnabled_', ''
    $selectorBefore = Invoke-McpTool $session gp_objects @{query="gpvst3GlobalPanel";limit=10}
    $result.selector_before_editor = @($selectorBefore.objects | Where-Object object_name -EQ "gpvst3GlobalPanel")[0].properties
    $editorButtons = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEditor_$classId");limit=10}
    if (@($editorButtons.objects).Count -eq 0) { throw 'P7 native editor button was not found.' }
    $editorButton = @($editorButtons.objects)[0]
    $result.editor_button = $editorButton
    $editorTrigger = Invoke-McpTool $session gp_trigger @{snapshot=$editorButtons.snapshot;id=$editorButton.id}
    Start-Sleep -Seconds 1
    $editorHost = Invoke-McpTool $session gp_objects @{query='gpvst3NativeEditorHost';limit=10}
    $result.editor_host = $editorHost
    $result.editor_trigger = $editorTrigger
    $editorHostObject = @($editorHost.objects | Where-Object object_name -EQ 'gpvst3NativeEditorHost')[0]
    if (-not $editorHostObject -or $editorHostObject.parent_name -ne 'gpvst3NativeEditorWindow') {
        throw "P7 native editor host is not in its independent window: $($editorHost | ConvertTo-Json -Depth 12 -Compress)"
    }
    $windows = Invoke-McpTool $session gp_windows @{include_hidden=$true}
    $editorWindow = @($windows.windows | Where-Object object_name -EQ 'gpvst3NativeEditorWindow')[0]
    if (-not $editorWindow.visible -or $editorWindow.kind -ne 'window' -or $editorWindow.modality -ne 'non_modal' -or
        $editorWindow.geometry.width -lt 100 -or $editorWindow.geometry.height -lt 100) { throw 'Native editor is not a visible independent nonmodal window.' }
    $result.editor_window = $editorWindow
    $selectorAfter = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalPanel';limit=10}
    $result.selector_after_editor = @($selectorAfter.objects | Where-Object object_name -EQ 'gpvst3GlobalPanel')[0].properties
    foreach ($property in @('x','y','width','height')) {
        if ($result.selector_before_editor.$property -ne $result.selector_after_editor.$property) { throw "Editor changed selector layout: $property" }
    }
    $screenshotBody = @{jsonrpc='2.0';id=3;method='tools/call';params=@{name='gp_screenshot';arguments=@{window_id=$editorWindow.window_id}}} | ConvertTo-Json -Depth 8 -Compress
    $screenshot = Invoke-RestMethod -Uri $session.Url -Method Post -Headers $session.Headers -ContentType 'application/json' -Body $screenshotBody
    $result.editor_screenshot = $screenshot.result.structuredContent
    $png = @($screenshot.result.content | Where-Object type -EQ 'image')[0]
    if ($png) { [IO.File]::WriteAllBytes((Join-Path $run 'editor.png'), [Convert]::FromBase64String($png.data)) }
    $result.native_window_capture = Save-P7NativeWindowCapture $process.Id $editorWindow.title (Join-Path $run 'editor-native.png')
    $result.editor_observation = (Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json).gp_hook
    if ($result.editor_observation.runtime_effect_error) {
        throw "P7 native editor reported a runtime error: $($result.editor_observation.runtime_effect_error)"
    }
    $result.editor_status = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalStatus';limit=10} -AllowError
    if (@($result.editor_status.objects | Where-Object { $_.properties.text -eq ('原生 GUI 已打开：' + $firstClass.name) }).Count -eq 0) {
        throw "P7 native editor status was not published: $($result.editor_status.objects.properties.text -join '; ')"
    }
    if ($CheckGain) {
        $baseline = Invoke-McpTool $session gp_objects @{query='gpvst3TestProcessor';limit=10}
        $baselineData = @($baseline.objects)[0].properties.text | ConvertFrom-Json
        $gain = Invoke-McpTool $session gp_objects @{query='gpvst3TestGain';limit=10}
        Invoke-McpTool $session gp_set_property @{snapshot=$gain.snapshot;id=@($gain.objects)[0].id;property='value';value=0.25} | Out-Null
        Start-Sleep -Seconds 1
        $changed = Invoke-McpTool $session gp_objects @{query='gpvst3TestProcessor';limit=10}
        $changedData = @($changed.objects)[0].properties.text | ConvertFrom-Json
        $result.gain_before = $baselineData
        $result.gain_after = $changedData
        if ($baselineData.instance -ne $changedData.instance -or $changedData.edits -le $baselineData.edits -or
            $changedData.input_energy -le 0.000001 -or $changedData.gain -ne 0.25 -or
            [Math]::Abs($changedData.output_energy / $changedData.input_energy - 0.0625) -gt 0.00001) { throw 'Editor edit did not reach the same processor and change actual output energy.' }
        $resize = Invoke-McpTool $session gp_objects @{query='gpvst3TestResize';limit=10}
        Invoke-McpTool $session gp_trigger @{snapshot=$resize.snapshot;id=@($resize.objects)[0].id} | Out-Null
        Start-Sleep -Milliseconds 300
        $moved = @((Invoke-McpTool $session gp_windows @{include_hidden=$true}).windows | Where-Object object_name -EQ 'gpvst3NativeEditorWindow')[0]
        $result.resized_moved_window = $moved
        if ($moved.geometry.width -ne 480 -or $moved.geometry.height -ne 240 -or $moved.geometry.x -eq $editorWindow.geometry.x) { throw 'Native IPlugFrame resize or independent movement failed.' }
    }
    $editorButtons2 = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEditor_$classId");limit=10}
    $editorButton2 = @($editorButtons2.objects)[0]
    $editorReopen = Invoke-McpTool $session gp_trigger @{snapshot=$editorButtons2.snapshot;id=$editorButton2.id}
    Start-Sleep -Milliseconds 500
    $result.editor_reopen = Invoke-McpTool $session gp_objects @{query='gpvst3NativeEditorHost';limit=10}
    if (@($result.editor_reopen.objects | Where-Object object_name -EQ 'gpvst3NativeEditorHost').Count -eq 0) {
        throw 'P7 native editor did not remain available after a repeated open request.'
    }
    $closeQuery = Invoke-McpTool $session gp_objects @{query='gpvst3NativeEditorWindow';limit=10}
    Invoke-McpTool $session gp_close_window @{snapshot=$closeQuery.snapshot;id=@($closeQuery.objects | Where-Object object_name -EQ 'gpvst3NativeEditorWindow')[0].id} | Out-Null
    Start-Sleep -Milliseconds 600
    $result.after_editor_close = (Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json).gp_hook
    if ($result.after_editor_close.runtime_effect_instances -ne 2 -or $result.after_editor_close.total_bypass -or $result.after_editor_close.runtime_process_count -le $result.editor_observation.runtime_process_count) { throw 'Closing editor disabled processing.' }
    $editorButtons = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEditor_" + $secondClass.class_id);limit=10}
    Invoke-McpTool $session gp_trigger @{snapshot=$editorButtons.snapshot;id=@($editorButtons.objects)[0].id} | Out-Null
    Start-Sleep -Milliseconds 600
    $result.second_editor_window = (Invoke-McpTool $session gp_windows @{include_hidden=$true}).windows | Where-Object object_name -EQ 'gpvst3NativeEditorWindow'
    if (-not $result.second_editor_window.visible -or $result.second_editor_window.title -notlike ($secondClass.name + '*')) { throw 'Second native editor did not open.' }
    $result.second_native_capture = Save-P7NativeWindowCapture $process.Id $result.second_editor_window.title (Join-Path $run 'second-editor-native.png')
    $closePanel = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalCloseSelectorButton';limit=10}
    Invoke-McpTool $session gp_trigger @{snapshot=$closePanel.snapshot;id=@($closePanel.objects)[0].id} | Out-Null
    Start-Sleep -Milliseconds 600
    $result.editor_after_panel_close = (Invoke-McpTool $session gp_windows @{include_hidden=$true}).windows | Where-Object object_name -EQ 'gpvst3NativeEditorWindow'
    if (-not $result.editor_after_panel_close.visible) { throw 'Closing selector destroyed the native editor.' }
    $entryAgain = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
    Invoke-McpTool $session gp_trigger @{snapshot=$entryAgain.snapshot;id=@($entryAgain.objects)[0].id} | Out-Null
    Start-Sleep -Seconds 1
    $uncheckedQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEnabled_$classId");limit=10}
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
    $secondOffQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEnabled_$(([string]$secondCheckbox.object_name) -replace '^gpvst3GlobalEnabled_', '')");limit=10}
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
    $savedStates = @($sidecar.global.effects | Where-Object { $_.component_state })
    if ($savedStates.Count -lt 2) {
        throw "P7 sidecar did not retain component state for both selected plugins: $($sidecar | ConvertTo-Json -Depth 8 -Compress)"
    }
    $result.sidecar = $sidecar
    $restoreQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEnabled_$classId");limit=10}
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
        $notice = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalStatus';limit=10}
        $result.restore_notice = $notice
        $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
        throw "P7 sidecar state did not restore a live processor: $($notice.objects.properties.toolTip -join '; '). Evidence: $run"
    }
    $restoreOffQuery = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEnabled_$classId");limit=10}
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
    if ($CheckGain -or $CheckCatalogRestart) {
        if ($CheckGain) {
        $enableForRestart = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEnabled_" + $firstClass.class_id);limit=10}
        Invoke-McpTool $session gp_set_property @{snapshot=$enableForRestart.snapshot;id=@($enableForRestart.objects)[0].id;property='checked';value=$true} | Out-Null
        Start-Sleep -Milliseconds 400
        $result.enabled_before_restart = Get-Content -LiteralPath (Join-Path $dataDirectory 'effect-chain.json') -Raw | ConvertFrom-Json
        }
        Invoke-McpTool $session gp_playback @{operation='set_loop';document=$operation.operation.document;enabled=[bool]$initialPlayback.loop} | Out-Null
        Invoke-McpTool $session gp_playback @{operation='stop';document=$operation.operation.document} | Out-Null
        $initialPlayback = $null
        if ($soundSectionBefore) { Set-P7SoundSection $session $soundSectionBefore | Out-Null; $soundSectionBefore = $null }
        Request-Gpvst3McpHostExit $session $process
        Start-Sleep -Milliseconds 700
        try { Close-McpSession $session } catch {}
        $session = $null
        $firstRun = Join-Path $run 'before-restart'
        New-Item -ItemType Directory -Force -Path $firstRun | Out-Null
        Stop-Gpvst3TestHost $process -RunDirectory $firstRun
        $process = $null
        $restartRun = Join-Path $run 'restart'
        $process = Start-Gpvst3TestHost -HostDirectory $HostDirectory -PluginPath $PluginPath -RunDirectory $restartRun -McpRoot $McpRoot -Environment $environment
        $sessionPath = Join-Path $restartRun 'mcp/native-session.json'
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        do {
            Start-Sleep -Milliseconds 100
            $restartStatus = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        } while (($restartStatus.pid -ne $process.Id -or -not (Test-Path -LiteralPath $sessionPath)) -and [DateTime]::UtcNow -lt $deadline)
        $result.restart_identity = Get-Gpvst3TestIdentity $process $HostDirectory $PluginPath $statusPath $McpRoot
        $session = New-McpSession -SessionFile $sessionPath
        Start-Sleep -Seconds 1
        if ($CheckGain) {
        $editor = Invoke-McpTool $session gp_objects @{query=("gpvst3GlobalEditor_" + $firstClass.class_id);limit=10}
        Invoke-McpTool $session gp_trigger @{snapshot=$editor.snapshot;id=@($editor.objects)[0].id} | Out-Null
        Start-Sleep -Milliseconds 400
        $gainRestored = Invoke-McpTool $session gp_objects @{query='gpvst3TestGain';limit=10}
        $restoredHook = (Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json).gp_hook
        $result.restart_gain = $gainRestored
        $result.restart_hook = $restoredHook
        if (@($gainRestored.objects)[0].properties.value -ne 0.25 -or $restoredHook.runtime_effect_instances -ne 1 -or
            $restoredHook.runtime_effect_error -or $restoredHook.total_bypass) { throw 'Enabled chain and processor state did not survive GP restart.' }
        } else {
            $cachedScan = (Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json).vst3_host
            if (-not $cachedScan.cache_hit -or $cachedScan.cache_reused -ne $scan.vst3_host.modules_discovered -or $cachedScan.modules_loaded -ne 0) { throw 'Default-directory restart failed to reuse the complete static cache.' }
            $open = Invoke-McpTool $session gp_open @{path=$fixture}
            $deadline = [DateTime]::UtcNow.AddSeconds(10)
            do {
                $operation = Invoke-McpTool $session gp_operation @{request=$open.request}
                if ($operation.operation.document) { break }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $deadline)
            Invoke-McpTool $session gp_activate @{document=$operation.operation.document} | Out-Null
            $soundSectionBefore = Set-P7SoundSection $session
            Start-Sleep -Milliseconds 600
            $entry = Invoke-McpTool $session gp_objects @{query='gpvst3SoundEffectChainButton';limit=10}
            $clock = [Diagnostics.Stopwatch]::StartNew()
            Invoke-McpTool $session gp_trigger @{snapshot=$entry.snapshot;id=@($entry.objects)[0].id} | Out-Null
            do {
                $panel = Invoke-McpTool $session gp_objects @{query='gpvst3GlobalPanel';limit=10}
                $visible = @($panel.objects | Where-Object object_name -EQ 'gpvst3GlobalPanel')[0].visible
                if ($visible) { break }
                Start-Sleep -Milliseconds 10
            } while ($clock.ElapsedMilliseconds -lt 500)
            $rows = Invoke-McpTool $session gp_objects @{query='gpvst3';limit=100}
            $checkboxes = @($rows.objects | Where-Object { $_.class -eq 'QCheckBox' -and $_.object_name -like 'gpvst3GlobalEnabled_*' })
            $openMs = $clock.ElapsedMilliseconds
            $process.Refresh()
            $loadedVst3 = @($process.Modules | Where-Object FileName -Like '*.vst3' | ForEach-Object FileName)
            $result.default_catalog_restart = @{scan=$cachedScan;list_open_ms=$openMs;vst3_modules=$loadedVst3;entry=$entry;panel=$panel;checkboxes=$checkboxes}
            $readyClasses = @($scan.vst3_catalog | Where-Object { $_.recognition_status -eq 'ready' -and $_.class_id })
            if ($openMs -gt 500 -or -not $visible -or $loadedVst3.Count -or $checkboxes.Count -ne $readyClasses.Count -or
                @($checkboxes | Where-Object { -not $_.visible -or -not $_.enabled }).Count) { throw "Cached default-directory list was slow, hidden, incomplete or loaded VST3 code. Time: $openMs ms; modules: $($loadedVst3.Count)." }
        }
    }
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
    Write-Output "PASS: P7 native global section, catalog and realtime selection lifecycle. Evidence: $run"
}
catch {
    if ($result) { $result.failure = $_.Exception.Message; $result.failure_stack = $_.ScriptStackTrace; $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8 }
    throw
}
finally {
    if ($session) {
        try {
            if ($soundSectionBefore) { Set-P7SoundSection $session $soundSectionBefore | Out-Null }
            if ($initialPlayback) {
                Invoke-McpTool $session gp_playback @{operation='stop';document=$operation.operation.document} | Out-Null
                Invoke-McpTool $session gp_playback @{operation='set_loop';document=$operation.operation.document;enabled=[bool]$initialPlayback.loop} | Out-Null
            }
            if (-not $KeepHost) {
                Request-Gpvst3McpHostExit $session $process
                Start-Sleep -Milliseconds 500
            }
        } catch { Write-Warning $_.Exception.Message }
        try { Close-McpSession $session } catch {}
    }
    try { Stop-Gpvst3TestHost $process -KeepHost:$KeepHost -RunDirectory $run }
    finally { Assert-Gpvst3HostUnchanged $before $HostDirectory $run }
}
