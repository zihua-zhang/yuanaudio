param(
    [Parameter(Mandatory = $true)][string]$StageRoot,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$stagePath = (Resolve-Path -LiteralPath $StageRoot).ProviderPath.TrimEnd('\')
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$uninstall = [Collections.Generic.List[string]]::new()
$preflight = [Collections.Generic.List[string]]::new()
$preflight.Add('!macro CheckInstalledFiles PREFIX')
# Check every packaged directory before inspecting/deleting any payload. A
# junction in plugins/sqldrivers/etc must not redirect installation elsewhere.
foreach ($directory in Get-ChildItem -LiteralPath $stagePath -Recurse -Directory) {
    $relative = [IO.Path]::GetRelativePath($stagePath, $directory.FullName).Replace('/', '\')
    if ($relative -match '[!$"\r\n]') { throw "Unsupported packaged directory name: $relative" }
    $preflight.Add(('  Push "$INSTDIR\{0}"' -f $relative))
    $preflight.Add('  Call ${PREFIX}CheckPayloadDirectory')
}
foreach ($file in Get-ChildItem -LiteralPath $stagePath -Recurse -File | Sort-Object FullName) {
    $relative = [IO.Path]::GetRelativePath($stagePath, $file.FullName).Replace('/', '\')
    if ($relative -match '[!$"\r\n]' -or $relative.StartsWith('..\')) {
        throw "Unsupported packaged file name: $relative"
    }
    $preflight.Add(('  Push "$INSTDIR\{0}"' -f $relative))
    $preflight.Add('  Call ${PREFIX}CheckFileUnlocked')
    $uninstall.Add(('!insertmacro DeleteInstalledFile "{0}"' -f $relative))
}
$preflight.Add('!macroend')
foreach ($directory in Get-ChildItem -LiteralPath $stagePath -Recurse -Directory |
    Sort-Object { $_.FullName.Length } -Descending) {
    $relative = [IO.Path]::GetRelativePath($stagePath, $directory.FullName).Replace('/', '\')
    if ($relative -match '[!$"\r\n]') { throw "Unsupported packaged directory name: $relative" }
    $uninstall.Add(('RMDir "$INSTDIR\{0}"' -f $relative))
}
# The old uninstaller may run in place during an upgrade. Its own executable
# is then removed by the new installer after ExecWait; all payload deletions
# above must succeed before registration/shortcuts are removed.
$uninstall.Add('Delete "$INSTDIR\Uninstall.exe"')
$uninstall.Add('RMDir "$INSTDIR"')
$uninstall | Set-Content -LiteralPath (Join-Path $OutputDirectory 'uninstall_files.nsh') -Encoding utf8
$preflight | Set-Content -LiteralPath (Join-Path $OutputDirectory 'preflight_files.nsh') -Encoding utf8
