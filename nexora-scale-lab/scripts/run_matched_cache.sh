#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export SCALE_LAB_COMPACT_CONFIG="$script_dir/../config/compact-matched-cache-100m.json"
exec bash "$script_dir/run_compact.sh" "$@"
