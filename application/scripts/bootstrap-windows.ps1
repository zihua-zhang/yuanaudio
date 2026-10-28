param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectPath
if (!(Test-Path -LiteralPath '.venv\Scripts\python.exe')) {
    & $Python -m venv .venv
    if ($LASTEXITCODE -ne 0) { throw 'Python environment creation failed' }
}
& '.\.venv\Scripts\python.exe' -m pip install 'aqtinstall==3.3.0' 'cmake==4.4.3' 'ninja==1.13.2'
if ($LASTEXITCODE -ne 0) { throw 'Build tool installation failed' }
if (!(Test-Path -LiteralPath '.tools\Qt\6.9.3\mingw_64\bin\Qt6Multimedia.dll') -or !(Test-Path -LiteralPath '.tools\Qt\6.9.3\mingw_64\bin\Qt6Core.dll')) {
    & '.\.venv\Scripts\python.exe' -m aqt install-qt windows desktop 6.9.3 win64_mingw -O .tools/Qt -m qtmultimedia --archives qtbase qtmultimedia
    if ($LASTEXITCODE -ne 0) { throw 'Qt installation failed' }
}
if (!(Test-Path -LiteralPath '.tools\Qt\Tools\mingw1310_64\bin\g++.exe')) {
    & '.\.venv\Scripts\python.exe' -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O .tools/Qt
    if ($LASTEXITCODE -ne 0) { throw 'Compiler installation failed' }
}
& '.\.venv\Scripts\python.exe' scripts/prepare-ffmpeg.py
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg header preparation failed' }
