param()
$ErrorActionPreference = 'Stop'

$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cmake = Get-Content -LiteralPath (Join-Path $project 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(AudioLibrary VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not read app version from CMakeLists.txt.'
}
$version = $Matches[1]
$buildRoot = [IO.Path]::GetFullPath((Join-Path $project 'build'))
$stageRoot = [IO.Path]::GetFullPath((Join-Path $buildRoot 'mac-source-package'))
if (!$stageRoot.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use a staging path outside build.'
}
if (Test-Path -LiteralPath $stageRoot) {
    $item = Get-Item -LiteralPath $stageRoot
    if ($item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
        (Resolve-Path -LiteralPath $stageRoot).ProviderPath -ne $stageRoot) {
        throw "Refusing to replace unexpected staging path: $stageRoot"
    }
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
$kitName = "YuanAudio-macOS27-arm64-source-$version"
$kitDir = Join-Path $stageRoot $kitName
New-Item -ItemType Directory -Force -Path $kitDir | Out-Null

foreach ($directory in @('.github', 'assets', 'docs', 'licenses', 'scripts', 'src', 'tests')) {
    Copy-Item -LiteralPath (Join-Path $project $directory) -Destination (Join-Path $kitDir $directory) -Recurse
}
foreach ($file in @('CMakeLists.txt', 'README.md', 'THIRD_PARTY_NOTICES.md', '.gitignore')) {
    Copy-Item -LiteralPath (Join-Path $project $file) -Destination (Join-Path $kitDir $file)
}
$quickStart = @"
源音效管理工具 · Mac 同事构建说明

要求：M 系列 Mac、macOS 27、Xcode 27 命令行工具、Python 3，网络可访问 Qt 和 FFmpeg 官网。

1. 解压此 ZIP，打开“终端”，进入解压后的 $kitName 文件夹。
2. 执行：bash scripts/prepare-macos.sh
3. 脚本完成后，请回传 dist 文件夹里的 DMG、同名 .sha256 文件和 macos-build.log。

构建失败时也请回传 macos-build.log；不要只截最后一行。
详细说明见 docs/macos-release.md。这个构建包不含公司的音乐或任何缓存。
"@
Set-Content -LiteralPath (Join-Path $kitDir '给Mac同事的操作说明.txt') -Value $quickStart -Encoding utf8

$dist = Join-Path $project 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zip = Join-Path $dist "源音效管理工具-macOS27-同事构建包-$version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($kitDir, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)

$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($file in Get-ChildItem -LiteralPath $kitDir -File -Recurse -Force) {
        $relative = [IO.Path]::GetRelativePath($kitDir, $file.FullName).Replace('\', '/')
        $entry = $archive.GetEntry("$kitName/$relative")
        if (!$entry -or $entry.Length -ne $file.Length) { throw "Source ZIP missing or changed: $relative" }
        $stream = $entry.Open()
        try {
            $actual = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream))
        } finally {
            $stream.Dispose()
        }
        if ($actual -ne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) {
            throw "Source ZIP hash mismatch: $relative"
        }
    }
} finally {
    $archive.Dispose()
}

$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$checksum = "$hash  $([IO.Path]::GetFileName($zip))"
Set-Content -LiteralPath "$zip.sha256" -Value $checksum -Encoding utf8
Write-Output "Mac source kit: $zip"
Write-Output "SHA-256: $hash"
