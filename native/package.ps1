param(
    [ValidatePattern('^\d+\.\d+\.\d+([-.][A-Za-z0-9.]+)?$')]
    [string]$Version = '0.8.0',
    [string]$PluginPath = '',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $PluginPath) { $PluginPath = Join-Path $root '.tools/native/plugins/imageformats/guitarpro_vst3_autoload.dll' }
if (-not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) {
    throw 'Build the plugin first with native/build.ps1 or pass -PluginPath explicitly.'
}
$PluginPath = (Resolve-Path -LiteralPath $PluginPath).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root ('artifacts/release-' + [guid]::NewGuid().ToString('N')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$staging = Join-Path $OutputDirectory ("GuitarProVST3-$Version")
if ((Test-Path -LiteralPath $staging) -or (Test-Path -LiteralPath "$staging.zip")) { throw 'Package output already exists. Choose a new -OutputDirectory.' }
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'plugins/imageformats'), (Join-Path $staging 'config'), (Join-Path $staging 'docs') | Out-Null

Copy-Item -LiteralPath $PluginPath -Destination (Join-Path $staging 'plugins/imageformats/guitarpro_vst3_autoload.dll')
Copy-Item -LiteralPath (Join-Path $root 'native/vst3-autoload.json') -Destination (Join-Path $staging 'plugins/imageformats/vst3-autoload.json')
Copy-Item -LiteralPath (Join-Path $root 'native/host_manifest.json') -Destination (Join-Path $staging 'config/host_manifest.json')
Copy-Item -LiteralPath (Join-Path $root 'native/effect-chain.template.json') -Destination (Join-Path $staging 'config/effect-chain.template.json')
# Release packages contain current surface docs; historical records stay in the source archive.
Copy-Item -LiteralPath (Join-Path $root 'docs/INSTALL.md'), (Join-Path $root 'docs/P8_IMPLEMENTATION.md'), (Join-Path $root 'docs/P8_TRACK_GLOBAL_VST3_PLAN.md'), (Join-Path $root 'docs/REALTIME_IMPLEMENTATION_PLAN.md'), (Join-Path $root 'docs/TESTING.md'), (Join-Path $root 'LICENSE') -Destination (Join-Path $staging 'docs')
Copy-Item -LiteralPath (Join-Path $root 'third_party/vst3sdk/LICENSE.txt') -Destination (Join-Path $staging 'docs/VST3-SDK-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'native/install.ps1'), (Join-Path $root 'native/uninstall.ps1') -Destination $staging

$files = @(Get-ChildItem -LiteralPath $staging -File -Recurse | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{path=$_.FullName.Substring($staging.Length + 1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
[ordered]@{schema=1;product='GuitarProVST3';version=$Version;host='Guitar Pro 8.1.1.17';files=$files} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $staging 'package.json') -Encoding UTF8
$zip = "$staging.zip"

# A release package must contain only the listed runtime/config/document files.
$forbidden = @(Get-ChildItem -LiteralPath $staging -File -Recurse | Where-Object {
    $relative = $_.FullName.Substring($staging.Length + 1)
    $relative -match '(^|\\)(artifacts|\.tools|third_party|\.cache)(\\|$)' -or $_.Extension -in @('.pdb','.obj','.lib')
})
if ($forbidden.Count) { throw "Forbidden release file: $($forbidden[0].FullName)" }
# Fix zip entry ordering and timestamps so the same payload yields the same zip.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $staging -File -Recurse | Sort-Object FullName) {
        $relative = $file.FullName.Substring($staging.Length + 1).Replace('\','/')
        $entry = $archive.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]::new(2000,1,1,0,0,0,[TimeSpan]::Zero)
        $input = [IO.File]::OpenRead($file.FullName)
        $output = $entry.Open()
        try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
    }
} finally { $archive.Dispose() }
$packageHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
@{package=$zip;sha256=$packageHash;files=$files;source_plugin_sha256=(Get-FileHash -LiteralPath $PluginPath -Algorithm SHA256).Hash} |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'verification.json') -Encoding UTF8
Write-Output "Package: $zip"
Write-Output "SHA256: $packageHash"
