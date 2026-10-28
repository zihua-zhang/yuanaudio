param([string]$ToolchainRoot)

$ErrorActionPreference = 'Stop'
$projectPath = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
if (!$ToolchainRoot) { $ToolchainRoot = $projectPath }
$ToolchainRoot = [IO.Path]::GetFullPath($ToolchainRoot)

$cmakeText = Get-Content -LiteralPath (Join-Path $projectPath 'CMakeLists.txt') -Raw
$versionMatch = [regex]::Match($cmakeText, 'project\(AudioLibrary\s+VERSION\s+(\d+\.\d+\.\d+)')
if (!$versionMatch.Success) { throw 'Cannot determine application version from CMakeLists.txt.' }
$version = $versionMatch.Groups[1].Value
$appExe = Join-Path $projectPath 'build\YuanAudio.exe'
if (!(Test-Path -LiteralPath $appExe)) { throw "Release executable not found: $appExe" }

$distRoot = Join-Path $projectPath 'dist'
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
$stage = [IO.Path]::GetFullPath((Join-Path $distRoot "YuanAudio-Windows-$version"))
$resolvedDist = (Resolve-Path -LiteralPath $distRoot).ProviderPath.TrimEnd('\')
if (!$stage.StartsWith($resolvedDist + '\', [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetDirectoryName($stage) -ne $resolvedDist) {
    throw "Unsafe package staging path: $stage"
}
if ((Get-Item -LiteralPath $distRoot).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
    throw 'Refusing to package into a reparse-point dist directory.'
}
if (Test-Path -LiteralPath $stage) {
    $existing = Get-Item -LiteralPath $stage
    if ($existing.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
        (Resolve-Path -LiteralPath $stage).ProviderPath -ne $stage) {
        throw "Refusing to remove unexpected staging path: $stage"
    }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item -LiteralPath $appExe -Destination $stage
$qtPath = Join-Path $ToolchainRoot '.tools\Qt\6.9.3\mingw_64'
$deployExe = Join-Path $qtPath 'bin\windeployqt.exe'
if (!(Test-Path -LiteralPath $deployExe)) { throw "Qt deployment tool not found: $deployExe" }
$env:PATH = "$qtPath\bin;$ToolchainRoot\.tools\Qt\Tools\mingw1310_64\bin;$env:PATH"
& $deployExe --release --force --no-translations --no-opengl-sw --compiler-runtime (Join-Path $stage 'YuanAudio.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
# Only SQLite is used by the catalog. Other SQL plugins pull optional client
# runtimes into the installer and are not needed on a clean editing workstation.
$sqlDrivers = Join-Path $stage 'sqldrivers'
if (!(Test-Path -LiteralPath (Join-Path $sqlDrivers 'qsqlite.dll'))) {
    throw 'Qt SQLite driver was not deployed.'
}
Get-ChildItem -LiteralPath $sqlDrivers -File -Filter '*.dll' |
    Where-Object Name -ne 'qsqlite.dll' |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

$readme = Join-Path $projectPath 'README.md'
if (Test-Path -LiteralPath $readme) { Copy-Item -LiteralPath $readme -Destination (Join-Path $stage '使用说明.md') }
$validation = Join-Path $projectPath "docs\validation-$version.md"
if (Test-Path -LiteralPath $validation) { Copy-Item -LiteralPath $validation -Destination (Join-Path $stage '版本说明.md') }
$notices = Join-Path $projectPath 'THIRD_PARTY_NOTICES.md'
if (Test-Path -LiteralPath $notices) { Copy-Item -LiteralPath $notices -Destination $stage }
$licenses = Join-Path $projectPath 'licenses'
if (Test-Path -LiteralPath $licenses) { Copy-Item -LiteralPath $licenses -Destination $stage -Recurse }

$stageFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
$manifest = [ordered]@{
    product = '源音效管理工具'
    version = $version
    executable = 'YuanAudio.exe'
    files = @($stageFiles | ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}
$manifestPath = Join-Path $distRoot "YuanAudio-Windows-$version-manifest.json"
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8

$zipPath = Join-Path $distRoot "源音效管理工具-免安装版-$version.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -LiteralPath $stage -DestinationPath $zipPath -CompressionLevel Optimal

$nsisCandidates = @(
    (Join-Path $projectPath '.tools\nsis-3.12\nsis-3.12\makensis.exe'),
    (Join-Path $projectPath '.tools\nsis-3.12\nsis-3.12\Bin\makensis.exe'),
    (Join-Path $ToolchainRoot '.tools\nsis-3.12\nsis-3.12\makensis.exe'),
    (Join-Path $ToolchainRoot '.tools\nsis-3.12\nsis-3.12\Bin\makensis.exe'),
    'C:\Program Files (x86)\NSIS\makensis.exe',
    'C:\Program Files\NSIS\makensis.exe'
)
$nsis = $nsisCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$nsis) { throw 'NSIS makensis.exe is required to create the installer. See scripts/README-packaging.md.' }

# Compile an uninstall list from exactly the files staged for this release. Never
# recursively remove the chosen installation directory: users may put files there.
$generatedDir = Join-Path $projectPath 'build\package'
New-Item -ItemType Directory -Force -Path $generatedDir | Out-Null
$uninstallList = Join-Path $generatedDir 'uninstall_files.nsh'
$preflightList = Join-Path $generatedDir 'preflight_files.nsh'
& (Join-Path $PSScriptRoot 'installer-file-lists.ps1') -StageRoot $stage -OutputDirectory $generatedDir

$iconPath = Join-Path $projectPath 'assets\YuanAudio.ico'
if (!(Test-Path -LiteralPath $iconPath)) { throw "Application icon not found: $iconPath" }
$installerPath = Join-Path $distRoot "源音效管理工具-安装版-$version.exe"
if (Test-Path -LiteralPath $installerPath) { Remove-Item -LiteralPath $installerPath -Force }
& $nsis /V2 /INPUTCHARSET UTF8 "/DVERSION=$version" "/DSOURCE_DIR=$stage" "/DOUTPUT_FILE=$installerPath" "/DICON_FILE=$iconPath" "/DUNINSTALL_LIST=$uninstallList" "/DPREFLIGHT_LIST=$preflightList" (Join-Path $PSScriptRoot 'installer.nsi')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }

$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    foreach ($file in $manifest.files) {
        $entry = $archive.GetEntry("YuanAudio-Windows-$version/$($file.path)")
        if (!$entry -or $entry.Length -ne $file.bytes) {
            throw "Portable ZIP is missing or changed: $($file.path)"
        }
        $stream = $entry.Open()
        try {
            $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant()
        } finally {
            $stream.Dispose()
        }
        if ($hash -ne $file.sha256) { throw "Portable ZIP hash mismatch: $($file.path)" }
    }
} finally {
    $archive.Dispose()
}

Write-Output "Portable ZIP: $zipPath"
Write-Output "Installer: $installerPath"
Write-Output "Manifest: $manifestPath"
