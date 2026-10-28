#!/usr/bin/env bash
set -euo pipefail
bundle_dir="$(cd "$(dirname "$0")" && pwd -P)"
exec bash "$bundle_dir/application/scripts/prepare-fixed-macos.sh"
