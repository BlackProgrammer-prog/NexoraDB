#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export PYTHONPATH="$(pwd)/..${PYTHONPATH:+:$PYTHONPATH}"
if [[ "${1:---plan}" == '--plan' ]]; then
  exec .venv/bin/python -m scale_lab protocol-plan --config config/memory-validation-10m.json
fi
if [[ ( "${1:-}" != '--run' && "${1:-}" != '--preflight' ) || "$(id -u)" != 0 ]]; then
  echo 'Use WSL root: bash scripts/run_memory_10m.sh --plan | --preflight | --run [new-results-directory]' >&2
  exit 2
fi
source .env.local
# Set before interpreter startup; inherited by spawned BSON producers.
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=131072
destination="${2:-results/memory10m_$(date -u +%Y%m%d_%H%M%S)}"
.venv/bin/python ../scripts/check_release_version.py
echo 'Checking native version and APIs before loading data.'
.venv/bin/python -c 'import nexoradb; from pathlib import Path; import runpy; expected=runpy.run_path(str(Path("../NexoraDB/src/nexoradb/_version.py")))["__version__"]; assert nexoradb.build_info()["project_version"]==expected, "Rebuild native: version mismatch"; assert all(hasattr(nexoradb.DocEngine,n) for n in ("memory_report","insert_many_bson_ingest","prepare_shutdown")), "Rebuild native: required APIs missing"'
echo 'Running 20k-record preflight: 2 repetitions, 24 cells, full validation and cleanup.'
.venv/bin/python -m scale_lab compare --config config/memory-preflight.json --directory "${destination}-preflight"
if [[ "$1" == '--preflight' ]]; then
  exit 0
fi
echo 'Nexora memory validation: 10M per repetition, two repetitions, full integrity checks.'
echo 'No time limit. Ctrl+C stops safely. No MongoDB/PostgreSQL jobs or 100M run.'
exec .venv/bin/python -m scale_lab compare --config config/memory-validation-10m.json --directory "$destination"
