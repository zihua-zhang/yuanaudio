#!/usr/bin/env bash
set -euo pipefail

# One-command bootstrap for a colleague's Apple Silicon Mac running macOS 27.
project_dir="$(cd "$(dirname "$0")/.." && pwd -P)"
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo '请在 macOS 27 的 M 系列 Mac 上运行此脚本。' >&2
  exit 1
fi
macos_version="$(sw_vers -productVersion)"
if [[ "${macos_version%%.*}" -lt 27 ]]; then
  echo "需要 macOS 27 或更新版本，当前为 $macos_version。" >&2
  exit 1
fi
command -v xcrun >/dev/null || { echo '请先安装 Xcode 27 命令行工具。' >&2; exit 1; }
sdk_version="$(xcrun --sdk macosx --show-sdk-version)"
if [[ "${sdk_version%%.*}" -lt 27 ]]; then
  echo "需要 macOS 27 SDK，当前为 $sdk_version。请安装 Xcode 27 命令行工具。" >&2
  exit 1
fi
command -v python3 >/dev/null || { echo '请先安装 Python 3。' >&2; exit 1; }
venv="$project_dir/.venv-macos"
if [[ ! -x "$venv/bin/python3" ]]; then
  python3 -m venv "$venv"
fi
"$venv/bin/python3" -m pip install --disable-pip-version-check \
  'aqtinstall==3.3.0' 'cmake>=3.24,<5' 'ninja>=1.11,<2'
qt_root="$project_dir/.tools/Qt"
qt_dir="$qt_root/6.11.2/macos"
if [[ ! -x "$qt_dir/bin/macdeployqt" ]]; then
  "$venv/bin/python3" -m aqt install-qt mac desktop 6.11.2 clang_64 \
    --outputdir "$qt_root" -m qtmultimedia
fi
mkdir -p "$project_dir/dist"
PATH="$venv/bin:$PATH" bash "$project_dir/scripts/build-macos.sh" "$qt_dir" \
  2>&1 | tee "$project_dir/dist/macos-build.log"
echo '构建完成。请回传 dist 中的 DMG、同名 .sha256 文件和 macos-build.log。'
