#!/usr/bin/env bash
# Explicit 1,000-record CLI integration check; data and results are retained.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source .env.local
export PYTHONPATH="$(pwd)/..${PYTHONPATH:+:$PYTHONPATH}"
run_id="$(date -u +%Y%m%d_%H%M%S)_$$"
namespace="scale_lab_cli_${run_id}"
output="results/cli_${run_id}"
for database in nexoradb postgresql mongodb; do
  common=(--database "$database" --namespace "$namespace" --records 1000)
  .venv/bin/python -m scale_lab prepare "${common[@]}" --output "$output/$database-prepare.json"
  .venv/bin/python -m scale_lab load "${common[@]}" --output "$output/$database-load.json"
  .venv/bin/python -m scale_lab run "${common[@]}" --clients 3 --operations 300 \
    --output "$output/$database-run.json"
done
