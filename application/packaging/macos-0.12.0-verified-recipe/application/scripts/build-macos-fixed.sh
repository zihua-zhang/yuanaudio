#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd -P)"
repair_dir="$(cd "$project_dir/.." && pwd -P)"
export YUANAUDIO_COCOA_PLUGIN="$repair_dir/qt-cocoa-build/plugins/platforms/libqcocoa.dylib"
YUANAUDIO_COCOA_SHA256="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["plugin_sha256"])' "$repair_dir/qt-cocoa-build/fix-manifest.json")"
export YUANAUDIO_COCOA_SHA256
exec bash "$project_dir/scripts/build-macos.sh" "$@"
