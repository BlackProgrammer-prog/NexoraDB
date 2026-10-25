#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export PYTHONPATH="$(pwd)/..${PYTHONPATH:+:$PYTHONPATH}"
if [[ "${1:---plan}" == '--plan' ]]; then
  exec .venv/bin/python -m scale_lab protocol-plan --config config/baseline-short.json
fi
if [[ "${1:-}" != '--run' || "$(id -u)" != 0 ]]; then
  echo 'Use WSL root: bash scripts/run_baseline_short.sh --plan | --run [new-results-directory]' >&2
  exit 2
fi
source .env.local
bash scripts/system/start-local-services.sh
destination="${2:-results/baseline_$(date -u +%Y%m%d_%H%M%S)}"
echo '1M per job, five independent repetitions per engine; not a 100M/publication claim.'
exec .venv/bin/python -m scale_lab compare --config config/baseline-short.json --directory "$destination"
