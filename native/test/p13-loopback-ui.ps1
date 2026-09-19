# Normal Qt controls plus a bounded long press for GP's lazy line-in popup.
# Call only for the collector-owned, manifest-verified test process.
if (-not ('P13LoopbackUiInput' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class P13LoopbackUiInput {
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rectangle);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
}
'@
}

function Get-P13ExactControl([string]$Name, [string]$Class = '') {
    $query = Invoke-McpTool $session gp_objects @{query=$Name;limit=30}
    $objects = @($query.objects | Where-Object { $_.object_name -ceq $Name -and (-not $Class -or $_.class -ceq $Class) })
    if ($objects.Count -ne 1) { throw "Expected exactly one observed $Name control; matched=$($objects.Count), truncated=$($query.truncated)." }
    return [pscustomobject]@{snapshot=$query.snapshot;control=$objects[0]}
}

function Set-P13LoopbackProperty([string]$Name, [string]$Property, [int]$Value, [string]$Phase) {
    $current = Get-P13ExactControl $Name
    $evidence = [ordered]@{phase=$Phase;object=$Name;property=$Property;before=$current.control.properties.$Property;desired=$Value;confirmed=$false}
    $result.loopback_input2.operations += $evidence
    if ($current.control.properties.$Property -eq $Value) { $evidence.confirmed=$true; return }
    if ($current.control.enabled -ne $true -or $Property -notin $current.control.writable_properties) {
        throw "Observed $Name.$Property is not writable/enabled during $Phase."
    }
    $evidence.trigger_attempted=$true
    $evidence.response=Invoke-McpTool $session gp_set_property @{snapshot=$current.snapshot;id=$current.control.id;property=$Property;value=$Value}
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    do {
        $current=Get-P13ExactControl $Name
        if ($current.control.properties.$Property -eq $Value) { $evidence.confirmed=$true; $evidence.readback=$current.control.properties; return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Readback did not confirm $Name.$Property=$Value during $Phase."
}

function Open-P13LineInPopup {
    $dialogs=Invoke-McpTool $session gp_dialogs
    if ($dialogs.blocked) { throw 'A modal dialog blocks line-in channel configuration.' }
    Invoke-McpTool $session gp_window @{state='restore'} | Out-Null
    $button=Get-P13ExactControl 'lineInButton' 'am::gui::ToggleButton'
    $popup=Get-P13ExactControl 'AMPopup_LineIn' 'am::gui::Popup'
    if ($popup.control.visible -eq $true) { return }
    $transport=Get-P13ExactControl 'transportBar' 'gp::gui::TransportBar'
    $main=Get-P13ExactControl 'MainWindow' 'gp::gui::MainWindow'
    $menus=Invoke-McpTool $session gp_objects @{query='QMenuBar';limit=20}
    $menu=@($menus.objects | Where-Object { $_.class -eq 'QMenuBar' -and $_.visible -eq $true })
    if ($menu.Count -ne 1) { throw 'The observed main-window menu bar is ambiguous.' }
    $b=$button.control.properties; $t=$transport.control.properties; $m=$menu[0].properties
    if ($button.control.enabled -ne $true -or $button.control.visible -ne $true -or
        $button.control.parent_name -cne 'transportBar' -or $t.x -ne 0 -or $t.y -ne 0 -or
        $m.x -ne 0 -or $m.y -ne 0 -or $m.height -le 0 -or $m.height -gt 64 -or
        $t.width -ne $main.control.properties.width -or $m.width -ne $t.width -or
        $b.width -le 0 -or $b.height -le 0 -or $b.x -lt 0 -or $b.y -lt 0 -or
        $b.x+$b.width -gt $t.width -or $b.y+$b.height -gt $t.height) {
        throw 'The observed GP 8.1.1.17 transport/menu layout does not match the validated long-press layout.'
    }
    $process.Refresh()
    if ($process.HasExited -or [string]$process.StartTime.ToFileTimeUtc() -cne $result.process_start_filetime -or $process.Path -ine $hostExe) {
        throw 'The collector-owned process identity changed before line-in UI input.'
    }
    $window=$process.MainWindowHandle
    $rect=New-Object P13LoopbackUiInput+Rect
    if ($window -eq [IntPtr]::Zero -or -not [P13LoopbackUiInput]::GetClientRect($window,[ref]$rect) -or
        $rect.Right-$rect.Left -ne $main.control.properties.width -or $rect.Bottom-$rect.Top -ne $main.control.properties.height) {
        throw 'Native window dimensions do not match the observed Qt main window.'
    }
    $point=New-Object P13LoopbackUiInput+Point
    $point.X=[int]($b.x+[Math]::Floor($b.width/2)); $point.Y=[int]($m.height+$b.y+[Math]::Floor($b.height/2))
    $clientPoint=[IntPtr](($point.Y -shl 16) -bor $point.X)
    if (-not [P13LoopbackUiInput]::ClientToScreen($window,[ref]$point)) { throw 'Could not resolve the observed line-in button to screen coordinates.' }
    if (-not $result.loopback_input2.cursor_before) {
        $previous=New-Object P13LoopbackUiInput+Point
        if (-not [P13LoopbackUiInput]::GetCursorPos([ref]$previous)) { throw 'Could not save the current cursor position.' }
        $result.loopback_input2.cursor_before=@{x=$previous.X;y=$previous.Y}
    }
    [P13LoopbackUiInput]::SetForegroundWindow($window) | Out-Null
    Start-Sleep -Milliseconds 200
    if (-not [P13LoopbackUiInput]::SetCursorPos($point.X,$point.Y)) { throw 'Could not position the cursor on the observed line-in button.' }
    try {
        if (-not [P13LoopbackUiInput]::PostMessage($window,0x201,[IntPtr]1,$clientPoint)) { throw 'Could not send the owned-window line-in press.' }
        Start-Sleep -Milliseconds 800
    } finally { [P13LoopbackUiInput]::PostMessage($window,0x202,[IntPtr]0,$clientPoint) | Out-Null }
    $deadline=[DateTime]::UtcNow.AddSeconds(3)
    do {
        $popup=Get-P13ExactControl 'AMPopup_LineIn' 'am::gui::Popup'
        if ($popup.control.visible -eq $true) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'The observed long press did not open the line-in settings popup.'
}

function Close-P13LineInPopup {
    $popup=Get-P13ExactControl 'AMPopup_LineIn' 'am::gui::Popup'
    if ($popup.control.visible -eq $true) {
        Invoke-McpTool $session gp_close_window @{snapshot=$popup.snapshot;id=$popup.control.id} | Out-Null
        $deadline=[DateTime]::UtcNow.AddSeconds(3)
        do {
            $popup=Get-P13ExactControl 'AMPopup_LineIn' 'am::gui::Popup'
            if ($popup.control.visible -ne $true) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($popup.control.visible -eq $true) { throw 'Line-in popup did not close.' }
    }
}

function Set-P13LoopbackInput2 {
    Open-P13LineInPopup
    $input=Get-P13ExactControl 'inputDeviceComboBox' 'am::gui::ComboBox'
    $gain=Get-P13ExactControl 'preGainVolumeSlider' 'am::gui::VolumeSlider'
    if ($input.control.properties.currentIndex -isnot [long] -and $input.control.properties.currentIndex -isnot [int]) { throw 'Input selector index is not an integer.' }
    $result.loopback_input2.original_input_index=[int]$input.control.properties.currentIndex
    $result.loopback_input2.original_input_text=$input.control.properties.currentText
    $result.loopback_input2.original_gain=[int]$gain.control.properties.value
    if ($result.loopback_input2.original_input_index -ne 0 -or $input.control.properties.count -ne 12 -or
        $null -eq $gain.control.properties.value -or $gain.control.properties.minimum -ne 0 -or $gain.control.properties.maximum -ne 1000) {
        throw 'Line-in control choices/range differ from the observed Studio 2 PRO configuration.'
    }
    Close-P13LineInPopup
    $result.loopback_input2.configuration_mutation_started=$true
    Set-P13NativeListener $true 'loopback_prepare_enable'
    Open-P13LineInPopup
    Set-P13LoopbackProperty 'preGainVolumeSlider' 'value' 0 'loopback_mute_input'
    $result.loopback_input2.muted_confirmed=$true
    Set-P13LoopbackProperty 'inputDeviceComboBox' 'currentIndex' 1 'loopback_select_input2'
    $selected=Get-P13ExactControl 'inputDeviceComboBox' 'am::gui::ComboBox'
    $result.loopback_input2.selected_input=$selected.control.properties
    Close-P13LineInPopup
    Set-P13NativeListener $false 'loopback_disable_native_monitor'
    $result.loopback_input2.configured_ns=[string](Get-P13ClockNanoseconds)
    $result.loopback_input2.configured=$true
}

function Restore-P13LoopbackInput {
    if (-not $result.loopback_input2.configuration_mutation_started -or
        $null -eq $result.loopback_input2.original_input_index -or $null -eq $result.loopback_input2.original_gain) { return }
    Close-P13LineInPopup
    # Gain stays at zero while the physical return input is selected. Re-enable
    # only to make GP's own selector writable, restore the channel before gain.
    Set-P13NativeListener $true 'loopback_restore_enable'
    Open-P13LineInPopup
    Set-P13LoopbackProperty 'preGainVolumeSlider' 'value' 0 'loopback_restore_mute'
    Set-P13LoopbackProperty 'inputDeviceComboBox' 'currentIndex' ([int]$result.loopback_input2.original_input_index) 'loopback_restore_input'
    Set-P13LoopbackProperty 'preGainVolumeSlider' 'value' ([int]$result.loopback_input2.original_gain) 'loopback_restore_gain'
    Close-P13LineInPopup
    Set-P13NativeListener ([bool]$result.native_listener.original_checked) 'loopback_restore_listener'
    $result.loopback_input2.restore_confirmed=$true
    $result.native_listener.restore_confirmed=$true
}

function Set-P13MonitorMeasurement {
    Open-P13LineInPopup
    $input=Get-P13ExactControl 'inputDeviceComboBox' 'am::gui::ComboBox'
    $gain=Get-P13ExactControl 'preGainVolumeSlider' 'am::gui::VolumeSlider'
    if ($input.control.properties.currentIndex -ne 0 -or $input.control.properties.count -ne 12 -or
        $null -eq $gain.control.properties.value -or $gain.control.properties.minimum -ne 0 -or $gain.control.properties.maximum -ne 1000) {
        throw 'Monitor measurement requires the observed Studio 2 PRO input-1 configuration.'
    }
    $result.loopback_input2.original_input_index=[int]$input.control.properties.currentIndex
    $result.loopback_input2.original_input_text=$input.control.properties.currentText
    $result.loopback_input2.original_gain=[int]$gain.control.properties.value
    $result.loopback_input2.configuration_mutation_started=$true
    Close-P13LineInPopup
    Set-P13NativeListener $true 'monitor_measurement_enable'
    Open-P13LineInPopup
    Set-P13LoopbackProperty 'preGainVolumeSlider' 'value' 0 'monitor_measurement_mute'
    $result.loopback_input2.muted_confirmed=$true
    # Previously observed index 2 is the native input 1/2 choice. Recheck its
    # label and actual driver mapping in this process before raising gain.
    $choices=@()
    foreach ($index in @(2)) {
        Set-P13LoopbackProperty 'inputDeviceComboBox' 'currentIndex' $index 'monitor_measurement_inspect'
        $observed=Get-P13ExactControl 'inputDeviceComboBox' 'am::gui::ComboBox'
        $choices += @{index=$index;text=$observed.control.properties.currentText}
    }
    $result.monitor_latency.input_choices=$choices
    $stereo=@($choices | Where-Object { $_.text -match '1\s*[/+&-]\s*2' })
    if ($stereo.Count -ne 1) { throw 'No unique observed input 1/2 stereo choice.' }
    Set-P13LoopbackProperty 'inputDeviceComboBox' 'currentIndex' ([int]$stereo[0].index) 'monitor_measurement_stereo'
    Close-P13LineInPopup
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do {
        $probe=Read-P13Json (Join-Path $dataDirectory 'p13-input-probe.json')
        if ($probe.monitor_latency_mapping_valid -eq $true) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($probe.monitor_latency_mapping_valid -ne $true) { throw 'Callback did not validate raw input 1/2 and source-only monitoring.' }
    Open-P13LineInPopup
    Set-P13LoopbackProperty 'preGainVolumeSlider' 'value' ([int]$result.loopback_input2.original_gain) 'monitor_measurement_restore_source_gain'
    Close-P13LineInPopup
    $result.monitor_latency.configured=$true
    $result.monitor_latency.configured_ns=[string](Get-P13ClockNanoseconds)
    $result.monitor_latency.native_gain=$result.loopback_input2.original_gain
}

function Restore-P13LoopbackCursor {
    if ($result.loopback_input2.cursor_before) {
        $saved=$result.loopback_input2.cursor_before
        $result.loopback_input2.cursor_restored=[P13LoopbackUiInput]::SetCursorPos([int]$saved.x,[int]$saved.y)
        if (-not $result.loopback_input2.cursor_restored) { throw 'Could not restore cursor position after line-in configuration.' }
    }
}
