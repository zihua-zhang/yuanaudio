param([switch]$CoreOnly, [switch]$Package, [string]$ToolchainRoot)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path -Parent $PSScriptRoot
if (!$ToolchainRoot) { $ToolchainRoot = $projectPath }
$qtPath = Join-Path $ToolchainRoot '.tools\Qt\6.9.3\mingw_64'
$compilerPath = Join-Path $ToolchainRoot '.tools\Qt\Tools\mingw1310_64\bin'
$env:PATH = "$qtPath\bin;$compilerPath;$ToolchainRoot\.venv\Scripts;$env:PATH"
$cmakePath = Join-Path $ToolchainRoot '.venv\Scripts\cmake.exe'
$appOption = if ($CoreOnly) { 'OFF' } else { 'ON' }
& $cmakePath -S $projectPath -B "$projectPath\build" -G Ninja '-UQt6*' "-DCMAKE_PREFIX_PATH=$qtPath" "-DFFMPEG_INCLUDE_DIR=$ToolchainRoot/.tools/ffmpeg-headers/include" "-DPython3_EXECUTABLE=$ToolchainRoot/.venv/Scripts/python.exe" '-DCMAKE_BUILD_TYPE=Release' "-DBUILD_DESKTOP_APP=$appOption" '-DCMAKE_CXX_COMPILER=g++'
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& $cmakePath --build "$projectPath\build" --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& "$ToolchainRoot\.venv\Scripts\ctest.exe" --test-dir "$projectPath\build" --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
if ($Package) {
    & (Join-Path $projectPath 'tests\installer_safety.ps1') -ToolchainRoot $ToolchainRoot
    if ($LASTEXITCODE -ne 0) { throw 'Installer safety checks failed' }
    & (Join-Path $PSScriptRoot 'package.ps1') -ToolchainRoot $ToolchainRoot
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
}
