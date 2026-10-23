#!/usr/bin/env bash
# Native capacity study, NOT a same-batch comparison with archived MongoDB.
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export SCALE_LAB_COMPACT_CONFIG="$script_dir/../config/ingestion-capacity-100m.json"
echo 'Nexora-only capacity profile: atomic durable batches of 10000, 2 BSON producer processes.'
echo 'Archived MongoDB results are untouched; this is NOT a direct engine ranking.'
exec bash "$script_dir/run_compact.sh" "$@"
