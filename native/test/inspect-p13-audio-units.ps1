param(
    [Parameter(Mandatory=$true)][ValidateRange(1, 2147483647)][int]$TargetProcessId,
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$ExpectedProcessStartFileTime = '',
    [string]$ExpectedPluginPath = '',
    [switch]$DumpFunctionBytes
)

# Read-only reconnaissance of the exact manifest host. No target writes,
# suspension, thread creation, code execution or injection are permitted here.
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell for the x64 host.' }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $root 'artifacts')).TrimEnd('\', '/')
$run = (Resolve-Path -LiteralPath $RunDirectory).Path.TrimEnd('\', '/')
if (-not $run.StartsWith($artifactRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunDirectory must be an existing child directory of this repository artifacts directory.'
}
$directory = Get-Item -LiteralPath $run
if (-not $directory.PSIsContainer) { throw 'RunDirectory is not a directory.' }
while ($directory) {
    if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse-point output ancestors are not permitted.' }
    $directory = $directory.Parent
}
$destination = Join-Path $run ('audio-units-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $destination | Out-Null
$outputPath = Join-Path $destination 'units.json'
if (-not ('P13AudioUnitReadOnlyMemory' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class P13AudioUnitReadOnlyMemory {
 [StructLayout(LayoutKind.Sequential)] public struct MemoryInfo {
  public IntPtr BaseAddress, AllocationBase;
  public uint AllocationProtect;
  public UIntPtr RegionSize;
  public uint State, Protect, Type;
 }
 [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] bytes, UIntPtr size, out UIntPtr read);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address, out MemoryInfo info, UIntPtr length);
 [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder name, ref uint size);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@
}
$result = [ordered]@{
    schema=1;status='inspection_failed';acceptance='not_evaluated';process_id=$TargetProcessId
    utc=[DateTime]::UtcNow.ToString('o');source='OpenProcess VM_READ | QUERY_INFORMATION; ReadProcessMemory and VirtualQueryEx only'
    concurrent_lifetime_race_possible=$true;maximum_units=8;maximum_function_pages=8;function_page_bytes=4096
    host_modules=@();units=@();listener_imports=@();errors=@();read_bytes=0;function_page_dumps=0
}
$handle = [IntPtr]::Zero
$process = $null
$moduleHashes = @{}
$dumpedPages = @{}
$dumpedMemoryPages = @{}
$modules = @()
$topologyObjects = @{}
$effectsRead = 0
$chainCache = @{}

function Register-P13TopologyObject([uint64]$Address, [string]$Kind) {
    if ($Address -lt 65536 -or $Address -ge 0x0000800000000000) { throw "Invalid $Kind object pointer." }
    $key = "$Kind/$Address"
    if (-not $topologyObjects.ContainsKey($key)) {
        if ($topologyObjects.Count -ge 128) { throw 'The 128 topology-object budget is exhausted.' }
        $topologyObjects[$key] = $true
    }
}

function Read-P13EffectsChain([uint64]$Address) {
    $chain = [ordered]@{address=('0x{0:X}' -f $Address);status='unknown';rails=@();concurrent_lifetime_race_possible=$true}
    if ($Address -eq 0) { $chain.status='not_present'; return [pscustomobject]$chain }
    if ($chainCache.ContainsKey([string]$Address)) { return $chainCache[[string]$Address] }
    try {
        Register-P13TopologyObject $Address 'EffectsChain'
        $header = Read-P13Memory $Address 16
        $vtable=[BitConverter]::ToUInt64($header,0); $impl=[BitConverter]::ToUInt64($header,8)
        $chain.vtable=Describe-P13Address $vtable
        if ($chain.vtable.module -notin @('GPRSE.dll','GuitarPro.exe')) { throw 'EffectsChain vtable is outside the verified native chain modules.' }
        Register-P13TopologyObject $impl 'EffectsChainImpl'
        $bytes=Read-P13Memory $impl 0xD0
        $chain.impl_address=('0x{0:X}' -f $impl)
        $strip=[BitConverter]::ToUInt64($bytes,0); $master=[BitConverter]::ToUInt64($bytes,0x88)
        $chain.channel_strip=Read-P13OptionalObject $strip
        $chain.master=Read-P13OptionalObject $master
        $truncated=$false
        for ($railIndex=0; $railIndex -lt 4; ++$railIndex) {
            $rail=[ordered]@{index=$railIndex;status='unknown';effects=@()}
            try {
                $offset=8+24*$railIndex
                $begin=[BitConverter]::ToUInt64($bytes,$offset)
                $end=[BitConverter]::ToUInt64($bytes,$offset+8)
                $capacity=[BitConverter]::ToUInt64($bytes,$offset+16)
                if ($end -lt $begin -or $capacity -lt $end -or (($end-$begin)%8) -ne 0 -or (($capacity-$begin)%8) -ne 0 -or
                    ($capacity-$begin) -gt 1048576 -or ($begin -eq 0 -and ($end -ne 0 -or $capacity -ne 0))) { throw 'Rail vector bounds are invalid.' }
                $count=[int](($end-$begin)/8)
                $limit=[Math]::Min(32,[Math]::Min($count,64-$script:effectsRead))
                $rail.count=$count; $rail.truncated=$limit -lt $count
                if ($rail.truncated) { $truncated=$true }
                $pointers=if ($limit) { Read-P13Memory $begin ($limit*8) } else { New-Object byte[] 0 }
                for ($effectIndex=0; $effectIndex -lt $limit; ++$effectIndex) {
                    ++$script:effectsRead
                    $effectAddress=[BitConverter]::ToUInt64($pointers,$effectIndex*8)
                    $effect=Read-P13OptionalObject $effectAddress
                    $rail.effects += [ordered]@{index=$effectIndex;object=$effect}
                }
                $after=Read-P13Memory ($impl+$offset) 24
                $before=New-Object byte[] 24; [Array]::Copy($bytes,$offset,$before,0,24)
                $rail.bounds_stable=[Convert]::ToBase64String($before) -ceq [Convert]::ToBase64String($after)
                $rail.status=if (-not $rail.bounds_stable -or $rail.truncated -or @($rail.effects|Where-Object {$_.object.status -notin @('read_unvalidated','not_present')}).Count) {'partial_or_unknown'} else {'read_unvalidated'}
            } catch { $rail.error=$_.Exception.Message }
            $chain.rails += [pscustomobject]$rail
        }
        $chain.root_header_stable=[Convert]::ToBase64String($header) -ceq [Convert]::ToBase64String((Read-P13Memory $Address 16))
        $chain.status=if (-not $chain.root_header_stable -or $truncated -or $chain.channel_strip.status -notin @('read_unvalidated','not_present') -or
            $chain.master.status -notin @('read_unvalidated','not_present') -or @($chain.rails|Where-Object status -NE 'read_unvalidated').Count) {'partial_or_unknown'} else {'read_unvalidated'}
    } catch { $chain.error=$_.Exception.Message }
    $chainCache[[string]$Address]=[pscustomobject]$chain
    return $chainCache[[string]$Address]
}

function Read-P13OptionalObject([uint64]$Address) {
    if ($Address -eq 0) { return [pscustomobject]@{address='0x0';status='not_present';vtable=$null} }
    try { Register-P13TopologyObject $Address 'native_object'; return Read-P13ObjectIdentity $Address }
    catch { return [pscustomobject]@{address=('0x{0:X}' -f $Address);status='unknown';error=$_.Exception.Message} }
}

function Read-P13SampleRateConverter([uint64]$Address) {
    $src=[ordered]@{address=('0x{0:X}' -f $Address);status='unknown';channels=@();concurrent_lifetime_race_possible=$true}
    if ($Address -eq 0) { $src.status='not_present'; return [pscustomobject]$src }
    try {
        Register-P13TopologyObject $Address 'StreamSampleRateConverter'
        $impl=Read-P13Pointer $Address
        Register-P13TopologyObject $impl 'StreamSampleRateConverterImpl'
        $src.impl_address=('0x{0:X}' -f $impl)
        for ($channel=0;$channel -lt 2;++$channel) {
            $resampler=$impl+$channel*0x78
            $item=[ordered]@{index=$channel;address=('0x{0:X}' -f $resampler);status='unknown';convolvers=@()}
            try {
                Register-P13TopologyObject $resampler 'resampler'
                $bytes=Read-P13Memory $resampler 0x58
                $vtable=[BitConverter]::ToUInt64($bytes,0)
                $item.vtable=Describe-P13Address $vtable
                if ($vtable -ne $audioModule.base+0x194608) { throw 'Unexpected resampler vtable.' }
                $count=[BitConverter]::ToInt32($bytes,0x48)
                $item.convolver_count=$count
                if ($count -lt 0 -or $count -gt 8) { throw 'Convolver count outside verified 0..8 bounds.' }
                for ($i=0;$i -lt $count;++$i) {
                    $conv=[BitConverter]::ToUInt64($bytes,8+8*$i)
                    $entry=[ordered]@{index=$i;address=('0x{0:X}' -f $conv);status='unknown'}
                    try {
                        Register-P13TopologyObject $conv 'convolver'
                        $cb=Read-P13Memory $conv 0x88
                        $cv=[BitConverter]::ToUInt64($cb,0)
                        $entry.vtable=Describe-P13Address $cv
                        if ($cv -ne $audioModule.base+0x194588) { throw 'Unexpected convolver vtable.' }
                        foreach ($field in @(@{name='up_factor';offset=0x28},@{name='down_factor';offset=0x2C},@{name='block_len2';offset=0x34},@{name='out_offset';offset=0x38},@{name='prev_input_len';offset=0x3C},@{name='input_len';offset=0x40},@{name='latency';offset=0x44},@{name='up_shift';offset=0x50},@{name='down_shift';offset=0x54},@{name='input_delay';offset=0x58},@{name='in_data_left';offset=0x80},@{name='latency_left';offset=0x84})) {
                            $entry[$field.name]=[BitConverter]::ToInt32($cb,$field.offset)
                        }
                        $entry.consume_latency=$cb[0x30] -ne 0
                        $entry.latency_fraction=[BitConverter]::ToDouble($cb,0x48)
                        $filter=[BitConverter]::ToUInt64($cb,8)
                        Register-P13TopologyObject $filter 'filter'
                        $fb=Read-P13Memory ($filter+0x48) 8
                        $entry.filter=[ordered]@{address=('0x{0:X}' -f $filter);kernel_len=[BitConverter]::ToInt32($fb,0);block_len_bits=[BitConverter]::ToInt32($fb,4)}
                        $entry.buffer_pointers=[ordered]@{previous_input=('0x{0:X}' -f [BitConverter]::ToUInt64($cb,0x68));current_input=('0x{0:X}' -f [BitConverter]::ToUInt64($cb,0x70));current_output=('0x{0:X}' -f [BitConverter]::ToUInt64($cb,0x78));contents_read=$false}
                        $entry.scalar_values_finite=-not ([double]::IsNaN($entry.latency_fraction) -or [double]::IsInfinity($entry.latency_fraction))
                        $entry.status=if ($entry.scalar_values_finite) {'read_unvalidated'} else {'partial_or_unknown'}
                    } catch { $entry.error=$_.Exception.Message }
                    $item.convolvers += [pscustomobject]$entry
                }
                $interpolator=[BitConverter]::ToUInt64($bytes,0x50)
                $interp=[ordered]@{address=('0x{0:X}' -f $interpolator);status='not_present'}
                if ($interpolator -ne 0) {
                    try {
                        Register-P13TopologyObject $interpolator 'interpolator'
                        $iv=Read-P13Pointer $interpolator
                        $interp.vtable=Describe-P13Address $iv
                        if ($iv -ne $audioModule.base+0x194648) { throw 'Unexpected interpolator vtable.' }
                        $ib=Read-P13Memory ($interpolator+0x1008) 0x40
                        foreach ($field in @(@{name='source_rate';offset=0},@{name='destination_rate';offset=8},@{name='initial_fraction';offset=0x10},@{name='input_position_fraction';offset=0x30},@{name='input_position_shift';offset=0x38})) { $interp[$field.name]=[BitConverter]::ToDouble($ib,$field.offset) }
                        foreach ($field in @(@{name='buffer_left';offset=0x18},@{name='write_position';offset=0x1C},@{name='read_position';offset=0x20},@{name='input_counter';offset=0x24},@{name='input_position_integer';offset=0x28})) { $interp[$field.name]=[BitConverter]::ToInt32($ib,$field.offset) }
                        $interp.scalar_values_finite=@($interp.source_rate,$interp.destination_rate,$interp.initial_fraction,$interp.input_position_fraction,$interp.input_position_shift | Where-Object { [double]::IsNaN($_) -or [double]::IsInfinity($_) }).Count -eq 0
                        $interp.status=if ($interp.scalar_values_finite) {'read_unvalidated'} else {'partial_or_unknown'}
                    } catch { $interp.status='unknown';$interp.error=$_.Exception.Message }
                }
                $item.interpolator=$interp
                $after=Read-P13Memory $resampler 0x58
                $item.topology_bytes_stable=[Convert]::ToBase64String($bytes) -ceq [Convert]::ToBase64String($after)
                $item.status=if (-not $item.topology_bytes_stable -or $interp.status -notin @('read_unvalidated','not_present') -or @($item.convolvers | Where-Object status -NE 'read_unvalidated').Count) {'partial_or_unknown'} else {'read_unvalidated'}
            } catch { $item.error=$_.Exception.Message }
            $src.channels += [pscustomobject]$item
        }
        $src.impl_pointer_stable=(Read-P13Pointer $Address) -eq $impl
        $src.status=if (-not $src.impl_pointer_stable -or @($src.channels | Where-Object status -NE 'read_unvalidated').Count) {'partial_or_unknown'} else {'read_unvalidated'}
    } catch { $src.error=$_.Exception.Message }
    $src.note='Remote reads are not an atomic callback snapshot; scalar counters can advance between reads. No SRC methods were called and no state was cleared.'
    return [pscustomobject]$src
}

function Read-P13OutputRing {
    $ring=[ordered]@{status='unknown';address=('0x{0:X}' -f ($audioModule.base+0x270810));snapshots=@();concurrent_lifetime_race_possible=$true}
    try {
        $channels=[BitConverter]::ToInt32((Read-P13Memory ($audioModule.base+0x2507F0) 4),0)
        $ring.channels=$channels
        for ($i=0;$i -lt 2;++$i) {
            $bytes=Read-P13Memory ($audioModule.base+0x270810) 0x40
            $capacity=[BitConverter]::ToUInt64($bytes,0); $mask=[BitConverter]::ToUInt64($bytes,8)
            $count=[BitConverter]::ToInt64($bytes,0x10)
            $read=[BitConverter]::ToUInt64($bytes,0x30); $write=[BitConverter]::ToUInt64($bytes,0x38)
            $valid=$capacity -eq 32768 -and $mask -eq 32767 -and $count -ge 0 -and $count -le $capacity -and
                $channels -in @(1,2) -and ($count % [Math]::Max(1,$channels)) -eq 0 -and $read -le $mask -and $write -le $mask
            $ring.snapshots += [ordered]@{capacity_samples=[string]$capacity;mask=[string]$mask;queued_samples=[string]$count;read_index=[string]$read;write_index=[string]$write;storage_address=('0x{0:X}' -f [BitConverter]::ToUInt64($bytes,0x18));observed_invariants_valid=$valid}
        }
        $ring.status=if (@($ring.snapshots|Where-Object observed_invariants_valid -NE $true).Count) {'partial_or_unknown'} else {'read_unvalidated'}
        $ring.note='Two unpaused remote snapshots; possible torn counters and concurrent writes. Capacity is not latency and no storage samples were read.'
    } catch { $ring.error=$_.Exception.Message }
    return [pscustomobject]$ring
}

function Read-P13Memory([uint64]$Address, [int]$Count) {
    if ($Address -lt 65536 -or $Address -ge 0x0000800000000000 -or $Count -le 0 -or $Count -gt 4096 -or
        $Address + [uint64]$Count -gt 0x0000800000000000 -or $result.read_bytes + $Count -gt 131072) {
        throw 'Rejected pointer, read size or aggregate read budget.'
    }
    $bytes = New-Object byte[] $Count
    $read = [UIntPtr]::Zero
    $result.read_bytes += $Count
    if (-not [P13AudioUnitReadOnlyMemory]::ReadProcessMemory($handle, [IntPtr][int64]$Address, $bytes, [UIntPtr][uint64]$Count, [ref]$read) -or $read.ToUInt64() -ne $Count) {
        throw ('ReadProcessMemory unavailable at 0x{0:X}, count {1}, Win32 {2}' -f $Address, $Count, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    return ,$bytes
}

function Read-P13Pointer([uint64]$Address) { return [BitConverter]::ToUInt64((Read-P13Memory $Address 8), 0) }

function Read-P13FunctionBytes([uint64]$Address) {
    $endAddress = $Address + 4096
    $module = $modules | Where-Object { $Address -ge $_.base -and $endAddress -le $_.base+$_.size } | Select-Object -First 1
    if (-not $module) { throw 'Function bytes extend outside one observed module.' }
    $firstPage = $Address - ($Address % 4096)
    $lastPage = ($endAddress - 1) - (($endAddress - 1) % 4096)
    $pages = @($firstPage)
    if ($lastPage -ne $firstPage) { $pages += $lastPage }
    $newPages = @($pages | Where-Object { -not $dumpedMemoryPages.ContainsKey([string]$_) })
    if ($dumpedMemoryPages.Count + $newPages.Count -gt 8) { throw 'The eight executable-memory-page dump budget is exhausted.' }
    foreach ($page in $pages) {
        $memory = [P13AudioUnitReadOnlyMemory+MemoryInfo]::new()
        $memorySize = [Runtime.InteropServices.Marshal]::SizeOf($memory)
        $queryBytes = [P13AudioUnitReadOnlyMemory]::VirtualQueryEx($handle, [IntPtr][int64]$page, [ref]$memory, [UIntPtr][uint64]$memorySize).ToUInt64()
        if ($queryBytes -eq 0) { throw 'VirtualQueryEx could not read the function-page protection.' }
        $segmentStart = [Math]::Max($Address, $page)
        $segmentEnd = [Math]::Min($endAddress, $page + 4096)
        $regionStart = [uint64]$memory.BaseAddress.ToInt64()
        $regionEnd = $regionStart + $memory.RegionSize.ToUInt64()
        if ($memory.State -ne 0x1000 -or ($memory.Protect -band 0x100) -ne 0 -or ($memory.Protect -band 0xF0) -eq 0 -or
            $segmentStart -lt $regionStart -or $segmentEnd -gt $regionEnd) { throw 'Function range includes an unavailable or non-executable page.' }
    }
    # Read from the function entry, not the preceding aligned page: this keeps
    # the listener tail at RVA 0x4FE60A inside the 0x4FDBD0 + 4096 byte dump.
    $bytes = Read-P13Memory $Address 4096
    foreach ($page in $pages) { $dumpedMemoryPages[[string]$page] = $true }
    return ,$bytes
}

function Read-P13ObjectIdentity([uint64]$Address, [switch]$AllowInactiveNull) {
    $identity = [ordered]@{address=('0x{0:X}' -f $Address);status='unknown';vtable=$null}
    if ($Address -eq 0 -and $AllowInactiveNull) {
        $identity.status='inactive_null'; $identity.note='Observed optional pointer is null; no object read attempted.'
        return [pscustomobject]$identity
    }
    try {
        $vtable=Read-P13Pointer $Address
        $identity.vtable=Describe-P13Address $vtable; $identity.status='read_unvalidated'
        # Optional MSVC x64 RTTI read; a missing name does not invent a DSP
        # identity. Every relative address must stay inside its verified module.
        try {
            $module=$modules | Where-Object { $vtable -ge $_.base + 8 -and $vtable + 8 -le $_.base + $_.size } | Select-Object -First 1
            $locator=Read-P13Pointer ($vtable - 8)
            if (-not $module -or $locator -lt $module.base -or $locator + 24 -gt $module.base + $module.size) { throw 'RTTI locator outside vtable module.' }
            $col=Read-P13Memory $locator 24
            $typeRva=[BitConverter]::ToUInt32($col,12); $selfRva=[BitConverter]::ToUInt32($col,20)
            if ([BitConverter]::ToUInt32($col,0) -ne 1 -or $locator - $selfRva -ne $module.base -or $typeRva + 272 -gt $module.size) { throw 'RTTI locator identity rejected.' }
            $name=[Text.Encoding]::ASCII.GetString((Read-P13Memory ($module.base + $typeRva + 16) 256)).Split([char]0)[0]
            if ($name -notmatch '^\.\?AV[^\x00-\x20]{1,250}@@$') { throw 'RTTI type name rejected.' }
            $identity.rtti_name=$name
        } catch { $identity.rtti_error=$_.Exception.Message }
    }
    catch { $identity.error=$_.Exception.Message }
    return [pscustomobject]$identity
}

function Describe-P13Address([uint64]$Address) {
    $module = $modules | Where-Object { $Address -ge $_.base -and $Address -lt $_.base + $_.size } | Select-Object -First 1
    $description = [ordered]@{address=('0x{0:X}' -f $Address);module=$null;rva=$null;path=$null;module_sha256=$null}
    if ($module) {
        if (-not $moduleHashes.ContainsKey($module.path)) { $moduleHashes[$module.path] = (Get-FileHash -LiteralPath $module.path -Algorithm SHA256).Hash }
        $description.module=$module.name; $description.rva=('0x{0:X}' -f ($Address - $module.base))
        $description.path=$module.path; $description.module_sha256=$moduleHashes[$module.path]
    }
    return [pscustomobject]$description
}

# This helper accepts only caller-provided, already identified module RVAs. It
# does not scan, guess offsets or execute the imported functions.
function Read-P13ImportTargets($Module, [uint64[]]$SlotRvas) {
    foreach ($rva in $SlotRvas) {
        $item = [ordered]@{slot_module=$Module.name;slot_rva=('0x{0:X}' -f $rva);status='unknown';target=$null}
        try {
            if ($rva + 8 -gt $Module.size) { throw 'Import slot is outside the verified module.' }
            $target = Read-P13Pointer ($Module.base + $rva)
            if ($target -lt 65536 -or $target -ge 0x0000800000000000) { throw 'Import target is not a valid user-space pointer.' }
            $item.target = Describe-P13Address $target
            $item.status = 'read_unvalidated'
        } catch { $item.error=$_.Exception.Message }
        [pscustomobject]$item
    }
}

try {
    $hostExe = (Resolve-Path -LiteralPath (Join-Path $HostDirectory 'GuitarPro.exe')).Path
    $process = Get-Process -Id $TargetProcessId
    if ($process.ProcessName -cne 'GuitarPro' -or $process.Path -ine $hostExe -or $process.HasExited -or
        (Get-Item -LiteralPath $hostExe).VersionInfo.FileVersion -ne '8.1.1.17') { throw 'Target is not the exact Guitar Pro 8.1.1.17 executable.' }
    $startFileTime = [string]$process.StartTime.ToFileTimeUtc()
    if ($ExpectedProcessStartFileTime -and $ExpectedProcessStartFileTime -cne $startFileTime) { throw 'Target process creation time does not match the collector.' }
    $modules = @($process.Modules | ForEach-Object {
        [pscustomobject]@{name=$_.ModuleName;path=$_.FileName;base=[uint64]$_.BaseAddress.ToInt64();size=[uint64]$_.ModuleMemorySize}
    })
    $manifest = Get-Content -LiteralPath (Join-Path $root 'native/host_manifest.json') -Raw | ConvertFrom-Json
    foreach ($entry in $manifest.files.PSObject.Properties) {
        $path = (Resolve-Path -LiteralPath (Join-Path $HostDirectory $entry.Name)).Path
        $loaded = @($modules | Where-Object { $_.name -ieq $entry.Name -and $_.path -ieq $path })
        if ($loaded.Count -ne 1) { throw "Manifest module is not loaded from the expected host directory: $($entry.Name)" }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -ine [string]$entry.Value) { throw "Manifest module hash mismatch: $($entry.Name)" }
        $moduleHashes[$path]=$hash
        $result.host_modules += [ordered]@{name=$entry.Name;path=$path;sha256=$hash;base=('0x{0:X}' -f $loaded[0].base);size=$loaded[0].size}
    }
    $handle = [P13AudioUnitReadOnlyMemory]::OpenProcess(0x410, $false, $TargetProcessId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $name = [Text.StringBuilder]::new(32768)
    $nameSize = [uint32]$name.Capacity
    $creation=[int64]0; $exited=[int64]0; $kernel=[int64]0; $userTime=[int64]0
    if (-not [P13AudioUnitReadOnlyMemory]::QueryFullProcessImageName($handle, 0, $name, [ref]$nameSize) -or $name.ToString() -ine $hostExe -or
        -not [P13AudioUnitReadOnlyMemory]::GetProcessTimes($handle, [ref]$creation, [ref]$exited, [ref]$kernel, [ref]$userTime) -or [string]$creation -cne $startFileTime -or $exited -ne 0) {
        throw 'Opened handle no longer matches the verified live process identity.'
    }
    $result.process_start_filetime=$startFileTime
    $audioModule = $modules | Where-Object name -IEQ 'AMAudio.dll' | Select-Object -First 1
    $rseModule = $modules | Where-Object name -IEQ 'GPRSE.dll' | Select-Object -First 1
    $exeModule = $modules | Where-Object path -IEQ $hostExe | Select-Object -First 1
    $expectedPlugin=$null
    if ($ExpectedPluginPath) {
        $expectedPath=(Resolve-Path -LiteralPath $ExpectedPluginPath).Path
        $expectedPlugin=@($modules | Where-Object { $_.path -ieq $expectedPath -and $_.name -ieq 'guitarpro_vst3_autoload.dll' })
        if ($expectedPlugin.Count -ne 1) { throw 'The explicitly expected plugin is not uniquely loaded.' }
        $result.expected_plugin=[ordered]@{path=$expectedPath;sha256=(Get-FileHash -LiteralPath $expectedPath).Hash}
    }
    $stream = Read-P13Pointer ($audioModule.base + 0x2F2620)
    $owner = Read-P13Pointer ($stream + 0x28)
    $parent = Read-P13Pointer $owner
    if ((Read-P13Pointer ($owner + 8)) -ne $stream -or (Read-P13Pointer ($parent + 0x178)) -ne $owner) { throw 'Stream/owner relationship changed or is invalid.' }
    $inputSrc=Read-P13Pointer ($owner+0x18); $outputSrc=Read-P13Pointer ($owner+0x20)
    $result.input_src=Read-P13SampleRateConverter $inputSrc
    $result.output_src=Read-P13SampleRateConverter $outputSrc
    $result.output_ring=Read-P13OutputRing
    $result.src_owner_pointers_stable=(Read-P13Pointer ($owner+0x18)) -eq $inputSrc -and (Read-P13Pointer ($owner+0x20)) -eq $outputSrc
    $vectorBefore = Read-P13Memory ($parent + 0x10) 16
    $begin=[BitConverter]::ToUInt64($vectorBefore, 0); $end=[BitConverter]::ToUInt64($vectorBefore, 8)
    if ($end -lt $begin -or (($end-$begin) % 16) -ne 0 -or ($end-$begin) -gt 1024) { throw 'AudioUnit vector size rejected.' }
    $unitCount = [int](($end-$begin)/16)
    $length = [Math]::Min($unitCount, 8) * 16
    $entries = if ($length) { Read-P13Memory $begin $length } else { New-Object byte[] 0 }
    $result.asio_stream=('0x{0:X}' -f $stream); $result.owner=('0x{0:X}' -f $owner); $result.parent=('0x{0:X}' -f $parent)
    $result.parent_vtable=Describe-P13Address (Read-P13Pointer $parent)
    $result.vector_count=$unitCount; $result.vector_truncated=$unitCount -gt 8
    for ($index=0; $index -lt ($length/16); ++$index) {
        $pointer = [BitConverter]::ToUInt64($entries, $index*16)
        $unit = [ordered]@{index=$index;object=('0x{0:X}' -f $pointer);status='unknown';concurrent_lifetime_race_possible=$true}
        try {
            $vtable=Read-P13Pointer $pointer; $fill=Read-P13Pointer ($vtable+8)
            $unit.vtable=Describe-P13Address $vtable; $unit.fill_buffer=Describe-P13Address $fill
            $expectedHook=$expectedPlugin -and $unit.fill_buffer.path -ieq $expectedPlugin[0].path -and
                $unit.fill_buffer.module_sha256 -ceq $result.expected_plugin.sha256
            $unit.expected_plugin_hook_observed=[bool]$expectedHook
            $unit.status='read_unvalidated'
            if ($vtable -eq $rseModule.base+0x23AD98 -and ($fill -eq $rseModule.base+0x492B0 -or $expectedHook)) {
                $identity=[ordered]@{status='partial_or_unknown';stage='unit_plus_10_to_conductor'}
                try {
                    $conductor=Read-P13Pointer ($pointer+0x10)
                    $identity.conductor=Read-P13ObjectIdentity $conductor -AllowInactiveNull
                    if ($conductor -eq 0) {
                        $identity.status='inactive_null'; $identity.stage='optional_conductor_absent'
                    } else {
                    $identity.stage='conductor_plus_10_to_impl'
                    $impl=Read-P13Pointer ($conductor+0x10)
                    $identity.conductor_impl_address=('0x{0:X}' -f $impl)
                    $master=$impl+0x60
                    $identity.embedded_master=Read-P13ObjectIdentity $master
                    $identity.stage='master_plus_10_to_impl'
                    $masterImpl=Read-P13Pointer ($master+0x10)
                    $identity.master_impl_address=('0x{0:X}' -f $masterImpl)
                    $identity.effects=@()
                    foreach ($field in @(@{name='eq';offset=0},@{name='limiter';offset=8},@{name='other_effect';offset=0x10},@{name='volume_pan_candidate';offset=0x18})) {
                        $effect=[ordered]@{name=$field.name;offset=('0x{0:X}' -f $field.offset);status='unknown'}
                        try { $effect.object=Read-P13ObjectIdentity (Read-P13Pointer ($masterImpl+$field.offset)); $effect.status=$effect.object.status }
                        catch { $effect.error=$_.Exception.Message }
                        $identity.effects += [pscustomobject]$effect
                    }
                    $identity.status=if ($identity.conductor.status -ne 'read_unvalidated' -or $identity.embedded_master.status -ne 'read_unvalidated' -or
                        @($identity.effects | Where-Object status -NE 'read_unvalidated').Count) { 'partial_or_unknown' } else { 'read_unvalidated' }
                    $identity.stage='bounded_identity_reads_finished'
                    }
                } catch { $identity.error=$_.Exception.Message }
                $unit.rse_master_identity=$identity
                if ($identity.status -notin @('read_unvalidated','inactive_null')) { $unit.status='partial_or_unknown' }
            }
            if ($DumpFunctionBytes -and $unit.fill_buffer.module) {
                try {
                $key = [string]$fill
                if (-not $dumpedPages.ContainsKey($key) -and $dumpedPages.Count -lt 8) {
                    $functionBytes = Read-P13FunctionBytes $fill
                    $dumpPath = Join-Path $destination ('function-entry-{0}.bin' -f $dumpedPages.Count)
                    [IO.File]::WriteAllBytes($dumpPath, $functionBytes)
                    $dumpedPages[$key]=[ordered]@{path=$dumpPath;base=('0x{0:X}' -f $fill);sha256=(Get-FileHash -LiteralPath $dumpPath).Hash;bytes=4096}
                }
                if ($dumpedPages.ContainsKey($key)) { $unit.function_bytes=$dumpedPages[$key] }
                } catch { $unit.function_page_error=$_.Exception.Message; $unit.status='partial_or_unknown' }
            }
            if ($vtable -eq $exeModule.base+0x25ABAF8) {
                # The experimental probe atomically replaces this vtable slot.
                # The exact native vtable still identifies the object layout;
                # a replaced method is recorded, never passed off as native.
                $unit.native_fill_entry_unchanged=$fill -eq $exeModule.base+0x4FDBD0
                if (-not $unit.native_fill_entry_unchanged -and -not $expectedHook) { $unit.status='partial_or_unknown' }
                $state = Read-P13Pointer ($pointer+8)
                $stateBytes = Read-P13Memory $state 0x200
                $statePath = Join-Path $destination ('listener-state-{0}.bin' -f $index)
                [IO.File]::WriteAllBytes($statePath, $stateBytes)
                $unit.listener_state=[ordered]@{address=('0x{0:X}' -f $state);dump_path=$statePath;bytes=512;sha256=(Get-FileHash -LiteralPath $statePath).Hash}
                $unit.listener_state.embedded_vtable_at_60=Describe-P13Address ([BitConverter]::ToUInt64($stateBytes, 0x60))
                $unit.listener_state.listener_chain=Read-P13EffectsChain ([BitConverter]::ToUInt64($stateBytes,0xC0))
                $unit.listener_state.source_chain=Read-P13EffectsChain ([BitConverter]::ToUInt64($stateBytes,0x1A8))
                if ($unit.listener_state.listener_chain.status -notin @('read_unvalidated','not_present') -or
                    $unit.listener_state.source_chain.status -notin @('read_unvalidated','not_present')) { $unit.status='partial_or_unknown' }
                foreach ($field in @(@{name='chain_at_c0';offset=0xC0}, @{name='effect_at_d0';offset=0xD0})) {
                    $childPointer=[BitConverter]::ToUInt64($stateBytes, $field.offset)
                    try {
                        $objectIdentity=Read-P13ObjectIdentity $childPointer -AllowInactiveNull:($field.name -eq 'chain_at_c0')
                        $unit.listener_state[$field.name]=$objectIdentity
                        if ($objectIdentity.status -notin @('read_unvalidated','inactive_null')) { $unit.status='partial_or_unknown' }
                    }
                    catch { $unit.listener_state[$field.name]=[ordered]@{address=('0x{0:X}' -f $childPointer);status='unknown';error=$_.Exception.Message}; $unit.status='partial_or_unknown' }
                }
            }
        } catch { $unit.status='partial_or_unknown'; $unit.error=$_.Exception.Message }
        $result.units += [pscustomobject]$unit
    }
    $slotRvas = [uint64[]]@(0xE300A8,0xE300B0,0xE300C0,0xE300C8,0xE300D0,0xE300D8,0xE300E0,0xE300E8,0xE300F0,0xE300F8,0xE30170,0xE303B0,0xE30668,0xE30670,0xE30678,0xE306A8,0xE306B0,0xE32238,0xE384D8,0xE384E0,0xE384E8,0xE384F0,0xE39268)
    $result.listener_imports = @(Read-P13ImportTargets $exeModule $slotRvas)
    $vectorAfter = Read-P13Memory ($parent+0x10) 16
    $entriesAfter = if ($length) { Read-P13Memory $begin $length } else { New-Object byte[] 0 }
    $result.vector_bounds_stable=[Convert]::ToBase64String($vectorBefore) -ceq [Convert]::ToBase64String($vectorAfter)
    $result.vector_entries_stable=[Convert]::ToBase64String($entries) -ceq [Convert]::ToBase64String($entriesAfter)
    $result.relationships_stable=(Read-P13Pointer ($audioModule.base+0x2F2620)) -eq $stream -and
        (Read-P13Pointer ($stream+0x28)) -eq $owner -and (Read-P13Pointer ($owner+8)) -eq $stream -and (Read-P13Pointer ($parent+0x178)) -eq $owner
    $result.inactive_null_conductors=@($result.units | Where-Object { $_.rse_master_identity.status -eq 'inactive_null' }).Count
    $result.inactive_null_listener_chains=@($result.units | Where-Object { $_.listener_state.chain_at_c0.status -eq 'inactive_null' }).Count
    $result.optional_null_note='Only the verified optional Conductor and listener-chain pointers accept inactive_null; invalid nonzero pointers and required objects remain unknown/errors.'
    $result.status = if (-not $result.vector_bounds_stable -or -not $result.vector_entries_stable -or -not $result.relationships_stable) { 'concurrent_change_observed' }
        elseif ($result.vector_truncated -or -not $result.src_owner_pointers_stable -or
            $result.input_src.status -notin @('read_unvalidated','not_present') -or $result.output_src.status -notin @('read_unvalidated','not_present') -or
            $result.output_ring.status -ne 'read_unvalidated' -or
            @($result.units | Where-Object status -NE 'read_unvalidated').Count -or @($result.listener_imports | Where-Object status -EQ 'unknown').Count) { 'partial_unvalidated' }
        else { 'collected_unvalidated' }
} catch { $result.errors += $_.Exception.Message; $result.status='inspection_failed' }
finally {
    if ($handle -ne [IntPtr]::Zero) { [void][P13AudioUnitReadOnlyMemory]::CloseHandle($handle) }
    if ($process) { $process.Dispose() }
    $result.function_page_dumps=$dumpedPages.Count
    $result.executable_memory_pages_dumped=$dumpedMemoryPages.Count
    $result.topology_objects_inspected=$topologyObjects.Count
    $result.rail_effects_inspected=$effectsRead
    $result.finished_utc=[DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $outputPath -Encoding UTF8
}
# Compact return value for callers; binary dumps and full JSON remain local.
[pscustomobject]@{status=$result.status;result_path=$outputPath;unit_count=@($result.units).Count;function_page_dumps=$result.function_page_dumps;errors=$result.errors}
