param(
    [string]$PluginPath = '',
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) { throw 'Build the plugin first with native/build.ps1.' }
$pluginSource = (Resolve-Path -LiteralPath $PluginPath).Path
if (-not $OutputRoot) { $OutputRoot = Join-Path $root 'artifacts' }
$OutputRoot = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('p6-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$packageOutput = Join-Path $OutputRoot 'release'
$version = '0.6.0-test'
& (Join-Path $PSScriptRoot 'package.ps1') -Version $version -PluginPath $PluginPath -OutputDirectory $packageOutput | Out-Host
$packageDirectory = Join-Path $packageOutput "GuitarProVST3-$version"
$manifest = Get-Content -LiteralPath (Join-Path $packageDirectory 'package.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.product -ne 'GuitarProVST3' -or @($manifest.files).Count -lt 6) { throw 'P6 package manifest is incomplete.' }
foreach ($file in $manifest.files) {
    $candidate = Join-Path $packageDirectory ([string]$file.path)
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf) -or
        (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -ine [string]$file.sha256) {
        throw "Package manifest hash mismatch: $($file.path)"
    }
}
$zip = "$packageDirectory.zip"
if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { throw 'Release zip was not produced.' }
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($archive.Entries | ForEach-Object FullName | Sort-Object)
    $expectedEntries = @(@($manifest.files.path) + 'package.json' | Sort-Object)
    if ((ConvertTo-Json $entries -Compress) -ne (ConvertTo-Json $expectedEntries -Compress)) { throw 'Release zip entries differ from package manifest.' }
} finally { $archive.Dispose() }

$hostFixture = Join-Path $OutputRoot 'host'
New-Item -ItemType Directory -Force -Path (Join-Path $hostFixture 'Plugins/imageformats') | Out-Null
[IO.File]::WriteAllText((Join-Path $hostFixture 'GuitarPro.exe'), 'p6 host fixture')
$installer = Join-Path $packageDirectory 'install.ps1'
$target = Join-Path $hostFixture 'Plugins/imageformats/guitarpro_vst3_autoload.dll'
$receipt = Join-Path $hostFixture 'Plugins/guitarpro-vst3-install.json'
try {
    [IO.File]::WriteAllText($target, 'unowned fixture')
    $unownedRejected = $false
    try { & $installer -Action Install -HostDirectory $hostFixture -PackageDirectory $packageDirectory | Out-Null }
    catch { $unownedRejected = $true }
    if (-not $unownedRejected) { throw 'Install overwrote an unowned existing plugin.' }
    Remove-Item -LiteralPath $target -Force
    $installed = & $installer -Action Install -HostDirectory $hostFixture -PackageDirectory $packageDirectory
    if (-not (Test-Path -LiteralPath $target) -or -not (Test-Path -LiteralPath $receipt)) { throw 'Package install did not create the owned plugin and receipt.' }
    $installedAgain = & $installer -Action Install -HostDirectory $hostFixture -PackageDirectory $packageDirectory
    if (-not $installedAgain.installed) { throw 'Idempotent package install failed.' }
    $status = & $installer -Action Status -HostDirectory $hostFixture -PackageDirectory $packageDirectory
    if (-not $status.installed) { throw 'Package status did not report the installed plugin.' }
    $originalHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    $stream = [IO.File]::Open($target, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $stream.WriteByte(0x50) } finally { $stream.Dispose() }
    $rejected = $false
    try { & $installer -Action Uninstall -HostDirectory $hostFixture -PackageDirectory $packageDirectory | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Uninstall accepted a modified owned file.' }
    Copy-Item -LiteralPath $pluginSource -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine $originalHash) { throw 'Package test could not restore the fixture plugin.' }
    & $installer -Action Uninstall -HostDirectory $hostFixture -PackageDirectory $packageDirectory | Out-Null
    if ((Test-Path -LiteralPath $target) -or (Test-Path -LiteralPath $receipt)) { throw 'Package uninstall left owned files.' }
} finally { }
Write-Output "PASS: P6 release package manifest, install ownership and uninstall safety. Evidence: $OutputRoot"
