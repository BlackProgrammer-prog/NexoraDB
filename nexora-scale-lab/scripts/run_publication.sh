#!/usr/bin/env bash
# Preparation is read-only by default. Explicit confirmation starts the real run.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export PYTHONPATH="$(pwd)/..${PYTHONPATH:+:$PYTHONPATH}"
python_bin="$(pwd)/.venv/bin/python"
if [[ "${1:-}" == '--plan' || $# == 0 ]]; then
  exec "$python_bin" -m scale_lab protocol-plan --config config/publication.json
fi
if [[ "${1:-}" != '--confirm-100m' ]]; then
  echo 'Usage: bash scripts/run_publication.sh --plan | --confirm-100m [new-results-directory]' >&2
  exit 2
fi
if [[ "$(id -u)" != 0 ]]; then
  echo 'Use root inside Ubuntu WSL for complete server IO/PSS profiling; see docs/methodology.md.' >&2
  exit 2
fi
source .env.local
bash scripts/system/prepare-publication-host.sh
destination="${2:-results/publication_$(date -u +%Y%m%d_%H%M%S)}"
exec "$python_bin" -m scale_lab compare --config config/publication.json \
  --directory "$destination" --allow-large
