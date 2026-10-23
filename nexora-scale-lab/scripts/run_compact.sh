#!/usr/bin/env bash
# 100M per engine, exploratory profile, no overall or per-job time limit.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export PYTHONPATH="$(pwd)/..${PYTHONPATH:+:$PYTHONPATH}"
python_bin="$(pwd)/.venv/bin/python"
config_file="${SCALE_LAB_COMPACT_CONFIG:-config/compact-100m.json}"
if [[ $# == 0 || "${1:-}" == '--plan' ]]; then
  exec "$python_bin" -m scale_lab protocol-plan --config "$config_file"
fi
if [[ ( "${1:-}" != '--confirm-100m' && "${1:-}" != '--resume' ) || "$(id -u)" != 0 ]]; then
  echo 'Use root inside WSL: bash scripts/run_compact.sh --confirm-100m [new-directory] | --resume existing-directory' >&2
  exit 2
fi
source .env.local
bash scripts/system/start-local-services.sh
destination="${2:-results/compact_$(date -u +%Y%m%d_%H%M%S)}"
resume_args=()
if [[ "$1" == '--resume' ]]; then
  [[ -n "${2:-}" ]] || { echo '--resume requires the existing results directory' >&2; exit 2; }
  resume_args=(--resume)
fi
echo 'No overall or per-job time limit. Press Ctrl+C to stop safely.'
echo "Results and detailed logs: $destination"
exec "$python_bin" -m scale_lab compare \
  --config "$config_file" --directory "$destination" --allow-large "${resume_args[@]}"
