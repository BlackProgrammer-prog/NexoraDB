#!/usr/bin/env bash
# Full-size dataset, preliminary workload only. Never a publication benchmark.
set -euo pipefail
if [[ "${1:-}" != '--confirm-100m' ]]; then
  echo 'Usage: bash scripts/run_100m_smoke.sh --confirm-100m [database|all]' >&2
  exit 2
fi
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source .env.local
export PYTHONPATH="$(pwd)/..${PYTHONPATH:+:$PYTHONPATH}"
python_bin="$(pwd)/.venv/bin/python"
run_id="$(date -u +%Y%m%d_%H%M%S)_$$"
namespace="scale_lab_100m_${run_id}"
output="results/${run_id}"
selected="${2:-all}"
case "$selected" in
  all) databases=(nexoradb postgresql mongodb) ;;
  nexoradb|postgresql|mongodb) databases=("$selected") ;;
  *) echo 'Unknown database' >&2; exit 2 ;;
esac
"$python_bin" -m scale_lab plan --records 100000000
# Conservatively budget the heuristic for all selected engines retained on disk.
"$python_bin" -c 'import shutil,sys; from scale_lab.dataset import DatasetConfig,record,canonical; c=DatasetConfig(); budget=sum(len(canonical(record(c,i))) for i in range(1000))*100000*4*int(sys.argv[1]); free=shutil.disk_usage(".").free; print(f"Planning disk budget: {budget/1e9:.1f} GB; available: {free/1e9:.1f} GB"); sys.exit(0 if free >= budget else "Insufficient estimated disk budget")' "${#databases[@]}"
mkdir -p "$output"
git -C .. rev-parse HEAD > "$output/commit.txt"
uname -a > "$output/kernel.txt"
lscpu > "$output/cpu.txt"
free -b > "$output/memory.txt"
echo "Full-size preliminary trial; namespace: $namespace; results: $output"
for database in "${databases[@]}"; do
  common=(--database "$database" --namespace "$namespace" --records 100000000 --allow-large)
  "$python_bin" -m scale_lab prepare "${common[@]}" --output "$output/$database-prepare.json"
  "$python_bin" -m scale_lab load "${common[@]}" --output "$output/$database-load.json"
  "$python_bin" -m scale_lab run "${common[@]}" --workload ycsb-c \
    --clients "${SCALE_LAB_CLIENTS:-10}" --operations "${SCALE_LAB_OPERATIONS:-100000}" \
    --output "$output/$database-ycsb-c.json"
done
echo "Completed. Data retained under $namespace. Results are preliminary, not publication eligible."
