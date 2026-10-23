#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export PYTHONPATH="$(pwd)/..${PYTHONPATH:+:$PYTHONPATH}"
if [[ "${1:---plan}" == '--plan' ]]; then
  exec .venv/bin/python -m scale_lab protocol-plan --config config/memory-validation-10m.json
fi
if [[ "${1:-}" != '--run' || "$(id -u)" != 0 ]]; then
  echo 'Use WSL root: bash scripts/run_memory_10m.sh --plan | --run [new-results-directory]' >&2
  exit 2
fi
source .env.local
# Set before interpreter startup; inherited by spawned BSON producers.
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=131072
destination="${2:-results/memory10m_$(date -u +%Y%m%d_%H%M%S)}"
echo 'Nexora memory validation: 10M per repetition, two repetitions, full integrity checks.'
echo 'No time limit. Ctrl+C stops safely. No MongoDB/PostgreSQL jobs or 100M run.'
exec .venv/bin/python -m scale_lab compare --config config/memory-validation-10m.json --directory "$destination"
