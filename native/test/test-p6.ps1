param(
    [string]$HostDirectory = 'C:\Program Files\Arobas Music\Guitar Pro 8',
    [string]$McpRoot = '',
    [string]$PluginPath = '',
    [string]$QtDir = '',
    [switch]$KeepHost,
    [switch]$SkipRuntime
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) { throw 'Build the plugin first with native/build.ps1.' }
$forbiddenTracked = @(git ls-files | Where-Object { $_ -match '(^|/)(artifacts|\.tools|\.cache)/|(^|/)(mcp-auth-token|native-session[^/]*)$|\.(dll|pdb|obj|lib)$' })
if ($forbiddenTracked.Count) { throw "Forbidden tracked release/build file: $($forbiddenTracked[0])" }
$sensitiveNames = @(Get-ChildItem -LiteralPath (Join-Path $root 'docs'), (Join-Path $root 'native') -File -Recurse |
    Where-Object { $_.Name -match '(?i)(token|secret|password|auth)' })
if ($sensitiveNames.Count) { throw "Sensitive-named file found in source tree: $($sensitiveNames[0].FullName)" }

$results = @()
& (Join-Path $PSScriptRoot 'test-p6-gate.ps1') -HostDirectory $HostDirectory -QtDir $QtDir | Tee-Object -Variable gateOutput | Out-Host
if ($LASTEXITCODE) { throw 'P6 host hash gate failed.' }
$results += [pscustomobject]@{suite='host_lock';result=($gateOutput -join ' ')}

& (Join-Path $PSScriptRoot 'test-p0.ps1') -HostDirectory $HostDirectory -PluginPath $PluginPath -KeepHost:$KeepHost | Tee-Object -Variable p0Output | Out-Host
if ($LASTEXITCODE) { throw 'P6 positive startup/uninstall regression failed.' }
$results += [pscustomobject]@{suite='startup_uninstall';result=($p0Output -join ' ')}

& (Join-Path $PSScriptRoot 'test-p0.ps1') -HostDirectory $HostDirectory -PluginPath $PluginPath -KeepHost:$KeepHost -TamperHostFile GuitarPro.exe | Tee-Object -Variable gateNegativeOutput | Out-Host
if ($LASTEXITCODE) { throw 'P6 negative host hash regression failed.' }
$results += [pscustomobject]@{suite='hash_negative';result=($gateNegativeOutput -join ' ')}

& (Join-Path $PSScriptRoot 'test-p4-router.ps1') | Tee-Object -Variable routerOutput | Out-Host
if ($LASTEXITCODE) { throw 'P6 input router regression failed.' }
$results += [pscustomobject]@{suite='input_router';result=($routerOutput -join ' ')}

& (Join-Path $PSScriptRoot 'test-p5-state.ps1') -QtDir $QtDir | Tee-Object -Variable stateOutput | Out-Host
if ($LASTEXITCODE) { throw 'P6 sidecar regression failed.' }
$results += [pscustomobject]@{suite='sidecar';result=($stateOutput -join ' ')}
& (Join-Path $PSScriptRoot 'test-p5-ui.ps1') -QtDir $QtDir | Tee-Object -Variable uiOutput | Out-Host
if ($LASTEXITCODE) { throw 'P6 Qt UI regression failed.' }
$results += [pscustomobject]@{suite='qt_ui';result=($uiOutput -join ' ')}

& (Join-Path $PSScriptRoot 'test-p6-package.ps1') -PluginPath $PluginPath | Tee-Object -Variable packageOutput | Out-Host
if ($LASTEXITCODE) { throw 'P6 release package regression failed.' }
$results += [pscustomobject]@{suite='package';result=($packageOutput -join ' ')}

if (-not $SkipRuntime) {
    if (-not $McpRoot) { $McpRoot = Join-Path (Split-Path -Parent $root) 'GuitarProMCP' }
    $runtimeFiles = @(
        (Join-Path $McpRoot '.tools/native/plugins/generic/guitarpro_mcp.dll'),
        (Join-Path $McpRoot '.tools/native/plugins/imageformats/guitarpro_mcp_autoload.dll'),
        (Join-Path $McpRoot 'native/mcp-client.ps1'),
        (Join-Path $HostDirectory 'GuitarPro.exe')
    )
    if ($runtimeFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }) {
        throw "P6 runtime regression prerequisites are missing under $McpRoot. Use -SkipRuntime only when recording a host-limited run."
    }
    & (Join-Path $PSScriptRoot 'test-p2-runtime.ps1') -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost -ExpectP3Fallback | Tee-Object -Variable faultOutput | Out-Host
    if ($LASTEXITCODE) { throw 'P6 abnormal-plugin fallback regression failed.' }
    $results += [pscustomobject]@{suite='plugin_exception_fallback';result=($faultOutput -join ' ')}
    & (Join-Path $PSScriptRoot 'test-p2-runtime.ps1') -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost -ExpectMissingPlugin | Tee-Object -Variable missingOutput | Out-Host
    if ($LASTEXITCODE) { throw 'P6 missing-plugin fallback regression failed.' }
    $results += [pscustomobject]@{suite='missing_plugin_fallback';result=($missingOutput -join ' ')}
    & (Join-Path $PSScriptRoot 'test-p2-runtime.ps1') -HostDirectory $HostDirectory -McpRoot $McpRoot -PluginPath $PluginPath -KeepHost:$KeepHost -P6Workflow | Tee-Object -Variable runtimeOutput | Out-Host
    if ($LASTEXITCODE) { throw 'P6 real-host workflow regression failed.' }
    $results += [pscustomobject]@{suite='real_host_workflow';result=($runtimeOutput -join ' ')}
} else {
    $results += [pscustomobject]@{suite='real_host_workflow';result='skipped_by_request; host-limited'}
}

$run = Join-Path $root ('artifacts/p6-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
@{complete=(-not $SkipRuntime);runtime_skipped=[bool]$SkipRuntime;results=$results;host_directory=(Resolve-Path $HostDirectory).Path;plugin_sha256=(Get-FileHash -LiteralPath $PluginPath -Algorithm SHA256).Hash;powershell=$PSVersionTable.PSVersion.ToString()} |
    ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $run 'verification.json') -Encoding UTF8
Write-Output "PASS: P6 version gate, regression, package and install ownership checks. Evidence: $run"
