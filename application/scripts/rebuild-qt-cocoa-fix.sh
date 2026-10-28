#!/usr/bin/env bash
set -euo pipefail
repair_dir="$(cd "$(dirname "$0")/../.." && pwd -P)"
qt_dir="${1:?Pass the exact official Qt 6.11.2 macos SDK directory}"
qt_dir="$(cd "$qt_dir" && pwd -P)"
cmake_bin="${CMAKE_BIN:-cmake}"
ninja_bin="$(command -v ninja)"
[[ "$("$qt_dir/bin/qmake" -query QT_VERSION)" == 6.11.2 ]] || exit 2
cd "$repair_dir"
mkdir -p sources logs
archive=sources/qtbase-v6.11.2.tar.gz
if [[ ! -f "$archive" ]]; then
 curl --fail --location --retry 3 https://codeload.github.com/qt/qtbase/tar.gz/refs/tags/v6.11.2 -o "$archive"
fi
[[ "$(shasum -a 256 "$archive" | awk '{print $1}')" == 06cd7aa6b3ab19cb5a1664a2891ef0df960f78c97fa31b80b1d6d7ee70688414 ]] || { echo 'Qt source archive mismatch' >&2; exit 1; }
if [[ ! -d sources/qtbase-6.11.2 ]]; then tar -xzf "$archive" -C sources; fi
patch="$repair_dir/handoff/源音效管理工具-Mac闪退修复资料-0.12.0/qt-upstream/qt-8148c48.patch"
[[ "$(shasum -a 256 "$patch" | awk '{print $1}')" == 494948a1d45b96c0ff16a3432cfde0f0ed205fa56ad923af1ab6c84aa07309b9 ]] || { echo 'Qt patch checksum mismatch' >&2; exit 1; }
# An enclosing application repository makes git apply silently skip paths outside
# its current subdirectory. The downloaded Qt tree must be treated independently.
apply_qt_patch() {
 env -u GIT_DIR -u GIT_WORK_TREE GIT_CEILING_DIRECTORIES="$repair_dir/sources" \
   git -C "$repair_dir/sources/qtbase-6.11.2" apply "$@" "$patch"
}
if apply_qt_patch --check; then
 apply_qt_patch
fi
# Fail closed if neither a fresh application nor a previous application exists.
apply_qt_patch --reverse --check
"$cmake_bin" -S qt-cocoa-project -B qt-cocoa-build -G Ninja  -DCMAKE_PREFIX_PATH="$qt_dir" -DCMAKE_MAKE_PROGRAM="$ninja_bin"  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_INSTALL_PREFIX="$repair_dir/no-install"  -DQT_SKIP_AUTO_PLUGIN_INCLUSION=ON -DQT_GENERATE_SBOM=OFF
"$cmake_bin" --build qt-cocoa-build --parallel 4
python3 - <<'PYCODE'
from pathlib import Path
import hashlib,json
p=Path('qt-cocoa-build/plugins/platforms/libqcocoa.dylib')
Path('qt-cocoa-build/fix-manifest.json').write_text(json.dumps({'qt_version':'6.11.2','upstream_commit':'8148c48deb8e4345bacb797cb75c011df6f797fa','plugin_sha256':hashlib.sha256(p.read_bytes()).hexdigest()},indent=2))
PYCODE
"$cmake_bin" -S sources/qtbase-6.11.2/tests/auto/other/qaccessibilitymac -B qt-native-tests -G Ninja  -DCMAKE_PREFIX_PATH="$qt_dir" -DCMAKE_MAKE_PROGRAM="$ninja_bin"  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
"$cmake_bin" --build qt-native-tests --parallel 4
QT_PLUGIN_PATH="$repair_dir/qt-cocoa-build/plugins" QT_DEBUG_PLUGINS=1 qt-native-tests/tst_qaccessibilitymac
