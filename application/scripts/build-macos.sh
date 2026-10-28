#!/usr/bin/env bash
set -euo pipefail

# Build and test on Apple Silicon macOS 27, then make a self-contained preview
# app and DMG. Qt's official qtmultimedia package supplies LGPL FFmpeg 7.1.3.
project_dir="$(cd "$(dirname "$0")/.." && pwd -P)"
qt_dir="${1:-${QT_DIR:-}}"
if [[ -z "$qt_dir" ]]; then
  echo 'Usage: build-macos.sh <Qt 6.11.2 macos directory> [--no-package]' >&2
  exit 2
fi
qt_dir="$(cd "$qt_dir" && pwd -P)"
package=true
if [[ "${2:-}" == '--no-package' ]]; then
  package=false
elif [[ -n "${2:-}" ]]; then
  echo "Unknown argument: $2" >&2
  exit 2
fi
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo 'This build must run on an Apple Silicon Mac.' >&2
  exit 1
fi
macos_version="$(sw_vers -productVersion)"
if [[ "${macos_version%%.*}" -lt 27 ]]; then
  echo "macOS 27 or newer is required to build and run this release's tests (found $macos_version)." >&2
  exit 1
fi
command -v xcrun >/dev/null || { echo 'Xcode 27 command line tools are required.' >&2; exit 1; }
sdk_version="$(xcrun --sdk macosx --show-sdk-version)"
if [[ "${sdk_version%%.*}" -lt 27 ]]; then
  echo "The macOS 27 SDK is required (found $sdk_version)." >&2
  exit 1
fi
for tool in cmake ninja curl shasum tar ctest otool lipo codesign hdiutil ditto python3; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
[[ -x "$qt_dir/bin/macdeployqt" ]] || { echo "macdeployqt missing under: $qt_dir" >&2; exit 1; }
[[ -f "$qt_dir/lib/cmake/Qt6/Qt6Config.cmake" ]] || { echo "Qt 6 development files missing under: $qt_dir" >&2; exit 1; }

ffmpeg_version=7.1.3
ffmpeg_sha256=f0bf043299db9e3caacb435a712fc541fbb07df613c4b893e8b77e67baf3adbe
source_cache="$project_dir/.tools/ffmpeg-macos"
archive="$source_cache/ffmpeg-$ffmpeg_version.tar.xz"
ffmpeg_source="$source_cache/ffmpeg-$ffmpeg_version"
mkdir -p "$source_cache"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 3 --output "$archive" \
    "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz"
fi
actual_sha256="$(shasum -a 256 "$archive" | awk '{ print $1 }')"
if [[ "$actual_sha256" != "$ffmpeg_sha256" ]]; then
  echo 'FFmpeg source checksum mismatch.' >&2
  exit 1
fi
if [[ ! -f "$ffmpeg_source/libavformat/avformat.h" ]]; then
  tar -xJf "$archive" -C "$source_cache"
fi
if [[ ! -f "$ffmpeg_source/libavutil/avconfig.h" ]]; then
  (cd "$ffmpeg_source" && ./configure --arch=arm64 --target-os=darwin \
    --cc=clang --disable-programs --disable-doc --disable-everything)
fi

build_dir="$project_dir/build-macos"
cmake -S "$project_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$qt_dir" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0 \
  -DFFMPEG_INCLUDE_DIR="$ffmpeg_source"
cmake --build "$build_dir" --parallel 4
ctest --test-dir "$build_dir" --output-on-failure

if [[ "$package" == false ]]; then
  echo "Built and tested: $build_dir/AudioLibrary.app"
  exit 0
fi

version="$(sed -nE 's/^project\(AudioLibrary VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$project_dir/CMakeLists.txt" | head -n 1)"
[[ -n "$version" ]] || { echo 'Cannot read application version from CMakeLists.txt.' >&2; exit 1; }
source_app="$build_dir/AudioLibrary.app"
[[ -d "$source_app" ]] || { echo "Built app bundle missing: $source_app" >&2; exit 1; }
dist_dir="$project_dir/dist"
mkdir -p "$dist_dir"
stage_name="YuanAudio-macOS27-arm64-$version.app"
stage_app="$dist_dir/$stage_name"
if [[ -e "$stage_app" || -L "$stage_app" ]]; then
  [[ ! -L "$stage_app" && "$stage_app" == "$project_dir/dist/"* ]] || {
    echo "Refusing to replace unexpected stage path: $stage_app" >&2; exit 1;
  }
  rm -rf -- "$stage_app"
fi
ditto "$source_app" "$stage_app"
(cd "$dist_dir" && "$qt_dir/bin/macdeployqt" "$stage_name" -verbose=1)
# The app opens SQLite only. Qt's other SQL drivers depend on optional Mimer,
# ODBC, or PostgreSQL libraries that are not present on a clean Mac.
sql_plugins="$stage_app/Contents/PlugIns/sqldrivers"
[[ -f "$sql_plugins/libqsqlite.dylib" ]] || { echo 'SQLite plugin missing from app bundle.' >&2; exit 1; }
rm -f -- "$sql_plugins/libqsqlmimer.dylib" "$sql_plugins/libqsqlodbc.dylib" "$sql_plugins/libqsqlpsql.dylib"

# Install the same-version, source-built accessibility fix after macdeployqt,
# before dependency validation and signing. Fail closed rather than silently
# repackaging the original Cocoa plugin on a later build.
: "${YUANAUDIO_COCOA_PLUGIN:?Use scripts/build-macos-fixed.sh with the rebuilt Qt 6.11.2 plugin}"
: "${YUANAUDIO_COCOA_SHA256:?Expected rebuilt plugin SHA-256 is required}"
python3 "$project_dir/scripts/deploy-patched-cocoa.py"   "$YUANAUDIO_COCOA_PLUGIN" "$YUANAUDIO_COCOA_SHA256" "$stage_app" "$qt_dir"

app_binary="$stage_app/Contents/MacOS/AudioLibrary"
[[ -f "$app_binary" ]] || { echo "App executable missing: $app_binary" >&2; exit 1; }
if [[ " $(lipo -archs "$app_binary") " != *' arm64 '* ]]; then
  echo 'The packaged executable does not contain arm64.' >&2
  exit 1
fi

# Reject links to development Qt, FFmpeg, or Homebrew locations. A fresh Mac
# must be able to load every non-system library from inside the app bundle.
while IFS= read -r -d '' binary; do
  if file -b "$binary" | grep -q 'Mach-O'; then
    lipo -verify_arch arm64 "$binary" || { echo "Bundled binary lacks arm64: $binary" >&2; exit 1; }
    while IFS= read -r dependency; do
      case "$dependency" in
        /System/Library/*|/usr/lib/*|@rpath/*|@loader_path/*|@executable_path/*) ;;
        *) echo "Undeployed library in $binary: $dependency" >&2; exit 1 ;;
      esac
    done < <(otool -arch arm64 -L "$binary" | awk 'NR > 1 { print $1 }')
    while IFS= read -r rpath; do
      case "$rpath" in
        @rpath/*|@loader_path/*|@executable_path/*|/System/Library/*|/usr/lib/*) ;;
        *) echo "External runtime search path in $binary: $rpath" >&2; exit 1 ;;
      esac
    done < <(otool -arch arm64 -l "$binary" | awk '/cmd LC_RPATH/ { in_rpath = 1; next } in_rpath && $1 == "path" { print $2; in_rpath = 0 }')
  fi
done < <(find "$stage_app/Contents" -type f -print0)
for ffmpeg_library in libavformat.61 libavcodec.61 libavutil.59; do
  if [[ -z "$(find "$stage_app/Contents" -name "$ffmpeg_library*.dylib" -print -quit)" ]]; then
    echo "Bundled FFmpeg library missing: $ffmpeg_library" >&2
    exit 1
  fi
done

legal_dir="$stage_app/Contents/Resources/Legal"
mkdir -p "$legal_dir"
cp "$project_dir/licenses/FFmpeg-LGPL-2.1.txt" "$legal_dir/"
cp "$project_dir"/licenses/Qt-*.txt "$legal_dir/"
cp "$project_dir/docs/macos-third-party.md" "$legal_dir/Third-Party-Notices.md"
cp "$project_dir/third_party/libsmb2/LICENCE-LGPL-2.1.txt" "$legal_dir/libsmb2-LGPL-2.1.txt"
cp "$project_dir/third_party/libsmb2/COPYING" "$legal_dir/libsmb2-COPYING"
cp "$project_dir/third_party/libsmb2/UPSTREAM.md" "$legal_dir/libsmb2-UPSTREAM.md"
# Ship the static library sources and application build inputs so this preview
# can be rebuilt/relinked independently, without a source-request round trip.
tar -czf "$legal_dir/Relink-Sources.tar.gz" -C "$project_dir" \
  CMakeLists.txt src assets tests scripts docs licenses third_party

if [[ -n "${APPLE_CODESIGN_IDENTITY:-}" ]]; then
  codesign --force --deep --options runtime --timestamp \
    --sign "$APPLE_CODESIGN_IDENTITY" "$stage_app"
else
  codesign --force --deep --sign - "$stage_app"
fi
codesign --verify --deep --strict --verbose=2 "$stage_app"
python3 "$project_dir/tests/decoder_regression.py" "$app_binary"

dmg_root="$build_dir/package/dmg-root"
if [[ -e "$dmg_root" || -L "$dmg_root" ]]; then
  [[ ! -L "$dmg_root" && "$dmg_root" == "$project_dir/build-macos/package/"* ]] || {
    echo "Refusing to replace unexpected DMG stage: $dmg_root" >&2; exit 1;
  }
  rm -rf -- "$dmg_root"
fi
mkdir -p "$dmg_root"
ditto "$stage_app" "$dmg_root/源音效管理工具.app"
ln -s /Applications "$dmg_root/Applications"
cp "$project_dir/docs/macos-release.md" "$dmg_root/安装与使用说明.md"
dmg="$dist_dir/源音效管理工具-macOS27-Apple芯片-$version.dmg"
if [[ -e "$dmg" || -L "$dmg" ]]; then
  [[ ! -L "$dmg" ]] || { echo "Refusing to overwrite symlink: $dmg" >&2; exit 1; }
  rm -- "$dmg"
fi
hdiutil create -volname "源音效管理工具 $version" -srcfolder "$dmg_root" \
  -ov -format UDZO "$dmg"
hdiutil verify "$dmg"
if [[ -n "${APPLE_NOTARY_PROFILE:-}" ]]; then
  [[ -n "${APPLE_CODESIGN_IDENTITY:-}" ]] || {
    echo 'APPLE_NOTARY_PROFILE requires APPLE_CODESIGN_IDENTITY.' >&2; exit 1;
  }
  xcrun notarytool submit "$dmg" --keychain-profile "$APPLE_NOTARY_PROFILE" --wait
  xcrun stapler staple "$dmg"
fi
(cd "$dist_dir" && shasum -a 256 "$(basename "$dmg")" > "$(basename "$dmg").sha256")
echo "macOS 27 Apple Silicon DMG: $dmg"
echo "Standalone app bundle: $stage_app"
