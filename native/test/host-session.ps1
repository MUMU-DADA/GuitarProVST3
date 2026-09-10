# Development loading follows GuitarProMCP/start-plugin.ps1: only the child
# environment changes; Guitar Pro and its installed plugins remain in place.
if (-not ('Gpvst3TestProcess' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Gpvst3TestProcess {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
}
'@
}

function Get-Gpvst3HostSnapshot([string]$HostDirectory) {
    $directory = (Resolve-Path -LiteralPath $HostDirectory).Path.TrimEnd('\', '/')
    $files = [ordered]@{}
    foreach ($file in Get-ChildItem -LiteralPath $directory -Recurse -File | Sort-Object FullName) {
        $files[$file.FullName.Substring($directory.Length + 1)] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $files
}

function Assert-Gpvst3HostUnchanged($Before, [string]$HostDirectory, [string]$RunDirectory) {
    $after = Get-Gpvst3HostSnapshot $HostDirectory
    $changed = @(@($Before.Keys) + @($after.Keys) | Sort-Object -Unique | Where-Object { $Before[$_] -cne $after[$_] })
    @{unchanged=($changed.Count -eq 0);host_directory=(Resolve-Path -LiteralPath $HostDirectory).Path;
      before=$Before;after=$after;changed=$changed} | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $RunDirectory 'host-integrity.json') -Encoding UTF8
    if ($changed.Count) { throw "Guitar Pro installation changed during the test: $($changed -join ', ')" }
}

function Start-Gpvst3TestHost {
    param(
        [string]$HostDirectory,
        [string]$PluginPath,
        [string]$RunDirectory,
        [string]$McpRoot = '',
        [hashtable]$Environment = @{},
        [switch]$Shortcut,
        [switch]$WithoutDevelopmentPlugin
    )
    $exe = (Resolve-Path -LiteralPath (Join-Path $HostDirectory 'GuitarPro.exe')).Path
    $directory = Split-Path -Parent $exe
    $RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
    New-Item -ItemType Directory -Force -Path $RunDirectory | Out-Null
    $pluginRoots = @()
    if (-not $WithoutDevelopmentPlugin) {
        $PluginPath = (Resolve-Path -LiteralPath $PluginPath).Path
        if ((Split-Path -Leaf (Split-Path -Parent $PluginPath)) -ne 'imageformats') {
            throw 'The development DLL must be in a Qt plugins/imageformats directory.'
        }
        if ($PluginPath.StartsWith($directory + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Use a development DLL outside the Guitar Pro installation.'
        }
        $pluginRoots += Split-Path -Parent (Split-Path -Parent $PluginPath)
    }
    if ($McpRoot) {
        $mcpPlugin = (Resolve-Path -LiteralPath (Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll')).Path
        $pluginRoots += Split-Path -Parent (Split-Path -Parent $mcpPlugin)
    }
    $names = @('QT_PLUGIN_PATH','QT_QPA_GENERIC_PLUGINS','TEMP','TMP') +
        @(Get-ChildItem Env: | Where-Object { $_.Name -like 'GPVST3_*' -or $_.Name -like 'GPMCP_*' } | ForEach-Object Name) +
        @('GPVST3_DATA_DIR','GPMCP_DATA_DIR','GPMCP_SESSION_FILE','GPMCP_BACKGROUND','GPMCP_DEVELOPMENT') + @($Environment.Keys)
    $saved = @{}
    foreach ($name in $names | Sort-Object -Unique) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    try {
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
        $env:QT_PLUGIN_PATH = $pluginRoots -join ';'
        if ($McpRoot) { $env:QT_QPA_GENERIC_PLUGINS = 'guitarpro_mcp' }
        $env:GPVST3_DATA_DIR = $RunDirectory
        # An already installed MCP bootstrap must also use test-local data.
        $env:GPMCP_DATA_DIR = Join-Path $RunDirectory 'mcp'
        $env:GPMCP_SESSION_FILE = Join-Path $RunDirectory 'mcp/native-session.json'
        $env:GPMCP_BACKGROUND = '1'
        $env:GPMCP_DEVELOPMENT = if ($McpRoot) { '1' } else { '0' }
        $env:TEMP = $RunDirectory
        $env:TMP = $RunDirectory
        foreach ($name in $Environment.Keys) { [Environment]::SetEnvironmentVariable($name, $Environment[$name], 'Process') }
        $launch = @{FilePath=$exe;WorkingDirectory=$directory;WindowStyle='Hidden';PassThru=$true}
        if ($Shortcut) {
            $shell = New-Object -ComObject WScript.Shell
            $link = $shell.CreateShortcut((Join-Path $RunDirectory 'Guitar Pro test.lnk'))
            $link.TargetPath = $exe
            $link.WorkingDirectory = $directory
            $link.Save()
            $launch.FilePath = $link.FullName
        } else {
            $launch.RedirectStandardError = Join-Path $RunDirectory 'host.stderr.log'
        }
        $process = Start-Process @launch
        $null = $process.Handle
        return $process
    } finally {
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
}

function Get-Gpvst3TestIdentity($Process, [string]$HostDirectory, [string]$PluginPath, [string]$StatusPath, [string]$McpRoot = '') {
    $Process.Refresh()
    if ($Process.HasExited) { throw "Test host exited ($($Process.ExitCode))." }
    $exe = (Resolve-Path -LiteralPath (Join-Path $HostDirectory 'GuitarPro.exe')).Path
    $plugin = (Resolve-Path -LiteralPath $PluginPath).Path
    $status = Get-Content -LiteralPath $StatusPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($Process.Path -ine $exe -or $status.pid -ne $Process.Id -or
        [IO.Path]::GetFullPath($status.executable) -ine $exe -or
        -not $status.plugin_path -or [IO.Path]::GetFullPath($status.plugin_path) -ine $plugin) {
        throw 'Test status does not belong to the original Guitar Pro process and requested development DLL.'
    }
    $mcp = if ($McpRoot) { (Resolve-Path -LiteralPath (Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll')).Path } else { '' }
    # Module enumeration can return a partial snapshot during startup loading.
    # Keep requiring the exact paths after refreshing that snapshot.
    $moduleDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $Process.Refresh()
        $modules = @($Process.Modules)
        $pluginLoaded = @($modules | Where-Object FileName -IEQ $plugin).Count -gt 0
        $mcpLoaded = -not $mcp -or @($modules | Where-Object FileName -IEQ $mcp).Count -gt 0
        if ($pluginLoaded -and $mcpLoaded) { break }
        if ([Gpvst3TestProcess]::WaitForSingleObject($Process.Handle, 0) -eq 0) { throw 'Test host exited while verifying loaded DLLs.' }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $moduleDeadline)
    if (-not $pluginLoaded -or -not $mcpLoaded) {
        $observed = @($modules | Where-Object ModuleName -Like 'guitarpro*' | ForEach-Object FileName)
        throw "Requested development DLLs were not loaded. Observed: $($observed -join '; ')"
    }
    $identity = [ordered]@{mode='original_host_no_install';pid=$Process.Id;executable=$exe;
        host_sha256=(Get-FileHash -LiteralPath $exe).Hash;plugin_path=$plugin;plugin_sha256=(Get-FileHash -LiteralPath $plugin).Hash}
    if ($McpRoot) {
        $identity.mcp_plugin_path = $mcp
        $identity.mcp_plugin_sha256 = (Get-FileHash -LiteralPath $mcp).Hash
    }
    return $identity
}

function Stop-Gpvst3TestHost($Process, [switch]$KeepHost, [string]$RunDirectory = '') {
    if (-not $Process) { return }
    $forced = $false
    $handle = $Process.Handle
    try {
        # ExitProcess publishes an exit code before DLL destructors finish.
        # HasExited / Stop-Process can miss a process stuck in those destructors.
        if (-not $KeepHost -and [Gpvst3TestProcess]::WaitForSingleObject($handle, 0) -ne 0) {
            try { $Process.CloseMainWindow() | Out-Null } catch { }
            if ([Gpvst3TestProcess]::WaitForSingleObject($handle, 5000) -ne 0) {
                $forced = $true
                if (-not [Gpvst3TestProcess]::TerminateProcess($handle, 1)) {
                    throw "Could not terminate test process $($Process.Id): Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
                }
                if ([Gpvst3TestProcess]::WaitForSingleObject($handle, 5000) -ne 0) { throw "Test process $($Process.Id) is still running after cleanup." }
            }
        }
    } finally {
        $exited = [Gpvst3TestProcess]::WaitForSingleObject($handle, 0) -eq 0
        if ($RunDirectory) {
            @{pid=$Process.Id;kept=[bool]$KeepHost;forced=$forced;exited=$exited;
              exit_code=$(if ($exited) { $Process.ExitCode } else { $null })} | ConvertTo-Json |
                Set-Content -LiteralPath (Join-Path $RunDirectory 'shutdown.json') -Encoding UTF8
        }
        $Process.Dispose()
    }
}
