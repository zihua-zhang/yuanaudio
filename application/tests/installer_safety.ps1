param([string]$ToolchainRoot)

# Build tiny NSIS fixtures against the real installer logic. Test mode fixes the
# only allowed destination and compiles out every registry/shortcut operation.
# This script never installs, upgrades or uninstalls the user's real application.
$ErrorActionPreference = 'Stop'
$projectPath = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (!$ToolchainRoot) { $ToolchainRoot = $projectPath }
$projectVersion = [regex]::Match((Get-Content -LiteralPath (Join-Path $projectPath 'CMakeLists.txt') -Raw),
    'project\(AudioLibrary\s+VERSION\s+(\d+\.\d+\.\d+)').Groups[1].Value
if (!$projectVersion) { throw 'Cannot determine installer fixture version.' }
$nsis = @(
    (Join-Path $projectPath '.tools\nsis-3.12\nsis-3.12\makensis.exe'),
    (Join-Path $projectPath '.tools\nsis-3.12\nsis-3.12\Bin\makensis.exe'),
    (Join-Path $ToolchainRoot '.tools\nsis-3.12\nsis-3.12\makensis.exe'),
    (Join-Path $ToolchainRoot '.tools\nsis-3.12\nsis-3.12\Bin\makensis.exe'),
    'C:\Program Files (x86)\NSIS\makensis.exe',
    'C:\Program Files\NSIS\makensis.exe'
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$nsis) { throw 'NSIS is required for installer safety checks.' }
$fixtureRoot = Join-Path $projectPath ('test-output\installer-fixture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
$fixtureRoot = (Resolve-Path -LiteralPath $fixtureRoot).ProviderPath
$results = [Collections.Generic.List[object]]::new()
$runningHelper = $null

function Assert-True([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}
function Run-Installer([string]$Executable, [string]$Arguments = '/S') {
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(20000)) {
        $process.Kill()
        throw "Fixture timed out: $Executable"
    }
    return $process.ExitCode
}
function Snapshot([string]$Directory) {
    $state = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Recurse -File) {
        $state[[IO.Path]::GetRelativePath($Directory, $file.FullName)] =
            (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $state | ConvertTo-Json -Compress
}
function Record([string]$Name) {
    $results.Add([ordered]@{ test = $Name; passed = $true })
    Write-Output "PASS: $Name"
}
function Start-Helper([string]$Directory) {
    $process = Start-Process -FilePath (Join-Path $Directory 'YuanAudio.exe') -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 150
    Assert-True (!$process.HasExited) 'Running-file fixture did not start.'
    return $process
}
function Stop-Helper($Process) {
    if ($Process -and !$Process.HasExited) { $Process.Kill(); $Process.WaitForExit() }
}
function Compile-Legacy([string]$InstallRoot, [string]$Output) {
    & $nsis /V2 /INPUTCHARSET UTF8 "/DFIXTURE_ROOT=$InstallRoot" "/DOUTPUT=$Output" "/DSTAGE=$oldStage" $legacyScript
    Assert-True ($LASTEXITCODE -eq 0) 'Legacy fixture compilation failed.'
}
function Compile-Current([string]$InstallRoot, [string]$Output) {
    & $nsis /V2 /INPUTCHARSET UTF8 "/DVERSION=$projectVersion" "/DSOURCE_DIR=$newStage" "/DOUTPUT_FILE=$Output" "/DICON_FILE=$projectPath\assets\YuanAudio.ico" "/DUNINSTALL_LIST=$lists\uninstall_files.nsh" "/DPREFLIGHT_LIST=$lists\preflight_files.nsh" "/DINSTALLER_TEST_ROOT=$InstallRoot" "$projectPath\scripts\installer.nsi"
    Assert-True ($LASTEXITCODE -eq 0) 'Current installer fixture compilation failed.'
}

try {
    $helperScript = Join-Path $fixtureRoot 'helper.nsi'
    @'
Unicode true
Name "Installer file-lock fixture"
OutFile "${OUTPUT}"
RequestExecutionLevel user
SilentInstall silent
Section
  Sleep 60000
SectionEnd
'@ | Set-Content -LiteralPath $helperScript -Encoding utf8
    $helperExe = Join-Path $fixtureRoot 'helper.exe'
    & $nsis /V2 "/DOUTPUT=$helperExe" $helperScript
    Assert-True ($LASTEXITCODE -eq 0) 'Helper compilation failed.'
    $oldStage = Join-Path $fixtureRoot 'old-stage'
    $newStage = Join-Path $fixtureRoot 'new-stage'
    foreach ($stage in @($oldStage, $newStage)) {
        New-Item -ItemType Directory -Path (Join-Path $stage 'plugins') -Force | Out-Null
        Copy-Item -LiteralPath $helperExe -Destination (Join-Path $stage 'YuanAudio.exe')
        Set-Content -LiteralPath (Join-Path $stage 'plugins\fixture.dll') -Value 'payload' -Encoding utf8
    }
    Set-Content -LiteralPath (Join-Path $oldStage 'version.txt') -Value 'old' -Encoding utf8
    Set-Content -LiteralPath (Join-Path $newStage 'version.txt') -Value 'new' -Encoding utf8
    $lists = Join-Path $fixtureRoot 'lists'
    & "$projectPath\scripts\installer-file-lists.ps1" -StageRoot $newStage -OutputDirectory $lists

    # Reproduce the old bare-Delete behavior with no real shell integration.
    $legacyScript = Join-Path $fixtureRoot 'legacy.nsi'
    @'
Unicode true
Name "Legacy installer fixture"
OutFile "${OUTPUT}"
InstallDir "${FIXTURE_ROOT}"
RequestExecutionLevel user
SilentInstall silent
SilentUnInstall silent
Section
  StrCmp $INSTDIR "${FIXTURE_ROOT}" +2
    Abort
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd
Section "Uninstall"
  StrCmp $INSTDIR "${FIXTURE_ROOT}" +2
    Abort
  Delete "$INSTDIR\YuanAudio.exe"
  Delete "$INSTDIR\version.txt"
  Delete "$INSTDIR\plugins\fixture.dll"
  RMDir "$INSTDIR\plugins"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
'@ | Set-Content -LiteralPath $legacyScript -Encoding utf8
    $proofRoot = Join-Path $fixtureRoot 'legacy-proof'
    $legacyProof = Join-Path $fixtureRoot 'legacy-proof-install.exe'
    Compile-Legacy $proofRoot $legacyProof
    Assert-True ((Run-Installer $legacyProof) -eq 0) 'Legacy proof installation failed.'
    $runningHelper = Start-Helper $proofRoot
    $legacyExit = Run-Installer (Join-Path $proofRoot 'Uninstall.exe') "/S _?=$proofRoot"
    Assert-True ($legacyExit -eq 0 -and (Test-Path -LiteralPath (Join-Path $proofRoot 'YuanAudio.exe')) -and !(Test-Path -LiteralPath (Join-Path $proofRoot 'version.txt'))) 'Old partial-uninstall bug was not reproduced.'
    Stop-Helper $runningHelper
    $runningHelper = $null
    Record 'Old uninstaller returns success after partial removal of a running app (reproduced safely)'

    $installRoot = Join-Path $fixtureRoot '安装 目录'
    $legacyInstaller = Join-Path $fixtureRoot 'legacy-upgrade-install.exe'
    $currentInstaller = Join-Path $fixtureRoot 'current-install.exe'
    Compile-Legacy $installRoot $legacyInstaller
    Compile-Current $installRoot $currentInstaller
    Assert-True ((Run-Installer $legacyInstaller) -eq 0) 'Legacy upgrade installation failed.'
    Set-Content -LiteralPath (Join-Path $installRoot 'user-music.keep') -Value 'preserve personal data' -Encoding utf8
    $runningHelper = Start-Helper $installRoot
    $before = Snapshot $installRoot
    Assert-True ((Run-Installer $currentInstaller) -eq 2) 'Running legacy app did not block upgrade.'
    Assert-True ((Snapshot $installRoot) -eq $before) 'Rejected legacy upgrade modified existing files.'
    Record 'Running old app blocks upgrade before invoking the old uninstaller; every file stays intact'
    Stop-Helper $runningHelper
    $runningHelper = $null

    Assert-True ((Run-Installer $currentInstaller) -eq 0) 'Closed legacy app did not upgrade.'
    Assert-True ((Get-Content -LiteralPath (Join-Path $installRoot 'version.txt') -Raw).Trim() -eq 'new') 'Upgrade did not deploy new payload.'
    Assert-True (Test-Path -LiteralPath (Join-Path $installRoot 'user-music.keep')) 'Upgrade removed unrelated user data.'
    Record 'Closed old app upgrades successfully; unrelated user data remains'

    $runningHelper = Start-Helper $installRoot
    $before = Snapshot $installRoot
    Assert-True ((Run-Installer $currentInstaller) -eq 2) 'Running current app did not block reinstall.'
    Assert-True ((Snapshot $installRoot) -eq $before) 'Rejected reinstall modified files.'
    Assert-True ((Run-Installer (Join-Path $installRoot 'Uninstall.exe') "/S _?=$installRoot") -eq 2) 'Running app did not block uninstall.'
    Assert-True ((Snapshot $installRoot) -eq $before) 'Rejected uninstall modified files.'
    Record 'Running current app blocks reinstall and uninstall without changing files'
    Stop-Helper $runningHelper
    $runningHelper = $null

    $locked = [IO.File]::Open((Join-Path $installRoot 'plugins\fixture.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $before = Snapshot $installRoot
        Assert-True ((Run-Installer $currentInstaller) -eq 2) 'Locked plugin did not block reinstall.'
        Assert-True ((Run-Installer (Join-Path $installRoot 'Uninstall.exe') "/S _?=$installRoot") -eq 2) 'Locked plugin did not block uninstall.'
        Assert-True ((Snapshot $installRoot) -eq $before) 'Locked plugin rejection modified files.'
    } finally { $locked.Dispose() }
    Record 'Locked deployed plugin blocks reinstall and uninstall before any payload deletion'

    $external = Join-Path $fixtureRoot 'outside-installation'
    New-Item -ItemType Directory -Path $external | Out-Null
    Set-Content -LiteralPath (Join-Path $external 'fixture.dll') -Value 'unrelated file' -Encoding utf8
    $plugins = Join-Path $installRoot 'plugins'
    Assert-True ($plugins.StartsWith($fixtureRoot + '\', [StringComparison]::OrdinalIgnoreCase)) 'Unsafe fixture rename.'
    Rename-Item -LiteralPath $plugins -NewName 'plugins.saved'
    New-Item -ItemType Junction -Path $plugins -Target $external | Out-Null
    try {
        $outsideBefore = Snapshot $external
        $before = Snapshot $installRoot
        Assert-True ((Run-Installer $currentInstaller) -eq 3) 'Nested junction did not block upgrade.'
        Assert-True ((Run-Installer (Join-Path $installRoot 'Uninstall.exe') "/S _?=$installRoot") -eq 3) 'Nested junction did not block uninstall.'
        Assert-True ((Snapshot $installRoot) -eq $before) 'Rejected junction operation changed installed files.'
        Assert-True ((Snapshot $external) -eq $outsideBefore) 'Junction redirected changes outside installation.'
    } finally {
        Assert-True ((Get-Item -LiteralPath $plugins).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) 'Fixture junction unexpectedly changed.'
        Remove-Item -LiteralPath $plugins -Force
        Rename-Item -LiteralPath (Join-Path $installRoot 'plugins.saved') -NewName 'plugins'
    }
    Record 'Nested directory junction blocks upgrade/uninstall and preserves outside files'
    $before = Snapshot $installRoot

    # NSIS itself ignores an invalid /D drive-root and retains the default.
    # A valid but unsafe Windows directory reaches our explicit path guard.
    Assert-True ((Run-Installer $currentInstaller "/S /D=$env:WINDIR") -eq 3) 'Unsafe system directory was not rejected.'
    Assert-True ((Snapshot $installRoot) -eq $before) 'Directory rejection changed the installation.'
    Record 'System-directory destination is rejected before modifications'

    Assert-True ((Run-Installer (Join-Path $installRoot 'Uninstall.exe') "/S _?=$installRoot") -eq 0) 'Closed app uninstall failed.'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $installRoot 'YuanAudio.exe'))) 'Payload remains after uninstall.'
    Assert-True ((Get-Content -LiteralPath (Join-Path $installRoot 'user-music.keep') -Raw).Trim() -eq 'preserve personal data') 'Uninstall removed unrelated data.'
    Record 'Successful uninstall removes payload and preserves unrelated user data'

    $badStage = Join-Path $fixtureRoot 'invalid-name-stage'
    New-Item -ItemType Directory -Path $badStage | Out-Null
    Set-Content -LiteralPath (Join-Path $badStage 'unsafe$.dll') -Value 'test'
    $rejected = $false
    try { & "$projectPath\scripts\installer-file-lists.ps1" -StageRoot $badStage -OutputDirectory (Join-Path $fixtureRoot 'invalid-lists') }
    catch { $rejected = $_.Exception.Message -like 'Unsupported packaged file name:*' }
    Assert-True $rejected 'NSIS variable-like file name was accepted.'
    Record 'Package list generation rejects NSIS-interpreted file names'
} finally {
    Stop-Helper $runningHelper
    $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $fixtureRoot 'results.json') -Encoding utf8
    Write-Output "Fixture report: $fixtureRoot\results.json"
}
