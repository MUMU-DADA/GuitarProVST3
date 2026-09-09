function Invoke-P6Workflow {
    param(
        [Parameter(Mandatory=$true)]$Session,
        [Parameter(Mandatory=$true)]$Document,
        [Parameter(Mandatory=$true)][string]$FixturePath,
        [Parameter(Mandatory=$true)][string]$RunDirectory,
        [Parameter(Mandatory=$true)]$Process
    )

    $ErrorActionPreference = 'Stop'
    $p6Context = @{ checks=0; evidence=@(); owned=@($Document); current=$Document }
    $deviceBefore = $null
    function Check-P6([bool]$condition, [string]$message) {
        if (-not $condition) { throw $message }
        $p6Context.checks++
    }
    function Call-P6([string]$name, [hashtable]$arguments = @{}, [switch]$AllowError) {
        $copy = $arguments.Clone()
        if ($p6Context.current) { $copy.document = $p6Context.current }
        Invoke-McpTool $Session $name $copy -AllowError:$AllowError
    }
    function Wait-P6([string]$request, [string]$expected) {
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            $state = (Invoke-McpTool $Session gp_operation @{request=$request}).operation
            if ($state.status -in @($expected, 'error', 'cancelled')) { break }
            Start-Sleep -Milliseconds 75
        } while ([DateTime]::UtcNow -lt $deadline)
        Check-P6 ($state.status -eq $expected) "P6 operation did not reach ${expected}: $($state | ConvertTo-Json -Depth 8 -Compress)"
        return $state
    }
    function Open-P6([string]$path) {
        $request = (Invoke-McpTool $Session gp_open @{path=$path}).request
        $opened = Wait-P6 $request 'opened'
        $p6Context.owned += $opened.document
        return $opened.document
    }
    function Close-P6([string]$id) {
        if (-not $id) { return }
        $result = Invoke-McpTool $Session gp_close @{document=$id;unsaved='discard'}
        if ($result.status -eq 'scheduled') { Wait-P6 $result.request 'closed' | Out-Null }
        $p6Context.owned = @($p6Context.owned | Where-Object { $_ -ne $id })
    }
    function Json-P6($value) { ConvertTo-Json -InputObject $value -Depth 20 -Compress }

    try {
        $beforeDocuments = @((Invoke-McpTool $Session gp_documents).documents | Sort-Object id | Select-Object id,dirty,opened_path,save_path)
        $initialPlayback = Call-P6 gp_playback
        Check-P6 (-not $initialPlayback.playing) 'P6 fixture was unexpectedly playing at start.'

        Call-P6 gp_playback @{operation='set_countdown';enabled=$false} | Out-Null
        Call-P6 gp_playback @{operation='play'} | Out-Null
        $playing = $false
        $deadline = [DateTime]::UtcNow.AddSeconds(8)
        do {
            $state = Call-P6 gp_playback
            $playing = [bool]$state.playing
            if (-not $playing) { Start-Sleep -Milliseconds 75 }
        } while (-not $playing -and [DateTime]::UtcNow -lt $deadline)
        Check-P6 $playing 'P6 playback did not start.'
        Call-P6 gp_playback @{operation='stop'} | Out-Null
        $stopped = $false
        $deadline = [DateTime]::UtcNow.AddSeconds(8)
        do {
            $state = Call-P6 gp_playback
            $stopped = -not [bool]$state.playing -and -not [bool]$state.counting_down
            if (-not $stopped) { Start-Sleep -Milliseconds 75 }
        } while (-not $stopped -and [DateTime]::UtcNow -lt $deadline)
        Check-P6 $stopped 'P6 playback did not stop.'
        Start-Sleep -Milliseconds 400
        $p6Context.evidence += @{name='play_stop';started=$playing;stopped=$stopped}

        $base = @{track=0;staff=0;voice=0;bar=0;beat=1}
        $extent = @{track=0;staff=0;voice=0;bar=0;beat=2}
        $range = $null
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        do {
            $range = Call-P6 gp_playback @{operation='set_loop_range';base=$base;extent=$extent} -AllowError
            if (-not $range.error) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        Check-P6 ($range.loop -and $range.range.loop_end_tick -gt $range.range.loop_start_tick) 'P6 loop range was not enabled.'
        Call-P6 gp_playback @{operation='play'} | Out-Null
        $ticks = @(); $previous = $null; $wrapped = $false
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        do {
            Start-Sleep -Milliseconds 75
            $loopState = Call-P6 gp_playback
            $ticks += $loopState.tick
            if ($null -ne $previous -and $loopState.tick -lt $previous) { $wrapped = $true }
            $previous = $loopState.tick
        } while (-not $wrapped -and [DateTime]::UtcNow -lt $deadline)
        Call-P6 gp_playback @{operation='stop'} | Out-Null
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        do {
            $loopStopped = Call-P6 gp_playback
            if (-not $loopStopped.playing -and -not $loopStopped.counting_down) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        $cleared = $null
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        do {
            $cleared = Call-P6 gp_playback @{operation='clear_loop_range'} -AllowError
            if (-not $cleared.error) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        Check-P6 (-not $cleared.error) 'P6 loop range could not be cleared after stopping playback.'
        Check-P6 $wrapped 'P6 loop playback did not wrap within the selected range.'
        $p6Context.evidence += @{name='loop';wrapped=$wrapped;samples=$ticks}

        $secondPath = Join-Path $RunDirectory 'p6-second-score.gp'
        Copy-Item -LiteralPath $FixturePath -Destination $secondPath -Force
        $second = Open-P6 $secondPath
        Invoke-McpTool $Session gp_activate @{document=$second} | Out-Null
        $otherState = Invoke-McpTool $Session gp_playback @{document=$second}
        Check-P6 ($otherState.document -eq $second -and -not $otherState.playing) 'P6 song change did not activate the second score.'
        $original = $p6Context.current
        Invoke-McpTool $Session gp_activate @{document=$original} | Out-Null
        $p6Context.evidence += @{name='song_change';second_document=$second;second_playing=[bool]$otherState.playing}

        $savedPath = Join-Path $RunDirectory 'p6-save-reopen.gp'
        $saved = Invoke-McpTool $Session gp_save_as @{document=$second;path=$savedPath}
        Check-P6 ($saved.status -eq 'saved' -and (Test-Path -LiteralPath $savedPath)) 'P6 save-as did not commit a GP file.'
        Close-P6 $second
        $p6Context.current = Open-P6 $savedPath
        Invoke-McpTool $Session gp_activate @{document=$p6Context.current} | Out-Null
        $reopened = @((Invoke-McpTool $Session gp_documents).documents | Where-Object id -eq $p6Context.current)
        Check-P6 ($reopened.Count -eq 1 -and $reopened[0].opened_path -eq $savedPath.Replace('\','/')) 'P6 save/reopen path did not round-trip.'
        $p6Context.evidence += @{name='save_reopen';path=$savedPath;document=$p6Context.current}
        Close-P6 $p6Context.current
        $p6Context.current = $original
        Invoke-McpTool $Session gp_activate @{document=$p6Context.current} | Out-Null

        $deviceBefore = Invoke-McpTool $Session gp_audio_device
        Check-P6 (-not $deviceBefore.error -and $deviceBefore.scope -eq 'application') 'P6 native audio device state unavailable.'
        $deviceResults = @()
        foreach ($backend in @('WASAPI','DirectSound','ASIO')) {
            $available = @($deviceBefore.choices.audioDevice | Where-Object { [string]$_ -ieq $backend })
            if ($available.Count) {
                $changed = Invoke-McpTool $Session gp_audio_device @{operation='set';property='audioDevice';value=$available[0]} -AllowError
                $deviceResults += @{backend=$backend;status=if ($changed.error) {'rejected'} else {'tested'};result=$changed}
                if (-not $changed.error) {
                    Invoke-McpTool $Session gp_audio_device @{operation='set';property='audioDevice';value=$deviceBefore.configuration.audioDevice} -AllowError | Out-Null
                }
            } else {
                $deviceResults += @{backend=$backend;status='host_choice_unavailable'}
            }
        }
        foreach ($property in @('audioBuffersSize','audioOutput','audioOutputChannels')) {
            if (-not $deviceBefore.choices.PSObject.Properties.Name.Contains($property)) { continue }
            $choice = @($deviceBefore.choices.$property | Where-Object { $_ -ne $deviceBefore.configuration.$property } | Select-Object -First 1)
            if (-not $choice.Count) { continue }
            $changed = Invoke-McpTool $Session gp_audio_device @{operation='set';property=$property;value=$choice[0]} -AllowError
            $deviceResults += @{property=$property;status=if ($changed.error) {'rejected'} else {'tested'};result=$changed}
            Invoke-McpTool $Session gp_audio_device @{operation='set';property=$property;value=$deviceBefore.configuration.$property} -AllowError | Out-Null
        }
        $p6Context.evidence += @{name='device_matrix';configured=$deviceBefore.configuration;choices=$deviceBefore.choices;results=$deviceResults;sample_rate='not_exposed_by_gp_audio_device';pause='not_exposed_by_gp_playback'}

        foreach ($id in @($p6Context.owned | Where-Object { $_ -ne $p6Context.current })) { Close-P6 $id }
        $afterDocuments = @((Invoke-McpTool $Session gp_documents).documents | Sort-Object id | Select-Object id,dirty,opened_path,save_path)
        Check-P6 ((Json-P6 $afterDocuments) -eq (Json-P6 $beforeDocuments)) 'P6 workflow changed an unrelated document.'

        $window = Invoke-McpTool $Session gp_objects @{query='MainWindow';limit=100}
        $main = @($window.objects | Where-Object class -eq 'gp::gui::MainWindow')
        Check-P6 ($main.Count -ge 1) 'P6 main window was not discoverable for clean exit.'
        try { Invoke-McpTool $Session gp_close_window @{snapshot=$window.snapshot;id=$main[0].id} | Out-Null } catch { }
        Check-P6 ($Process.WaitForExit(30000)) 'P6 host did not exit after closing the main window.'
        Check-P6 ($Process.ExitCode -eq 0) "P6 host exited with code $($Process.ExitCode)."
        return [pscustomobject]@{complete=$true;checks=$p6Context.checks;evidence=$p6Context.evidence;exit_code=$Process.ExitCode;device_matrix=$deviceResults}
    } finally {
        if ($deviceBefore) {
            try {
                $p6Context.currentDevice = Invoke-McpTool $Session gp_audio_device
                foreach ($property in @('audioDevice','audioOutput','audioInput','audioOutputChannels','audioBuffersSize')) {
                    if ($deviceBefore.configuration.PSObject.Properties.Name.Contains($property) -and
                        $p6Context.currentDevice.configuration.$property -ne $deviceBefore.configuration.$property) {
                        Invoke-McpTool $Session gp_audio_device @{operation='set';property=$property;value=$deviceBefore.configuration.$property} -AllowError | Out-Null
                    }
                }
            } catch { }
        }
        foreach ($id in @($p6Context.owned | Where-Object { $_ -ne $p6Context.current })) { try { Close-P6 $id } catch { } }
    }
}
