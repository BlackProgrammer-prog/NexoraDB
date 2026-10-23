#!/usr/bin/env bash
# Called only after explicit full-run confirmation; never from --plan.
set -euo pipefail
if [[ "$(id -u)" != 0 ]]; then
  echo 'Root inside Ubuntu WSL is required.' >&2
  exit 2
fi
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd -- "$script_dir/../.." && pwd)"
mkdir -p "$lab/state"
exec 9>>"$lab/state/protocol.lock"
flock -n 9 || { echo 'A protocol run is active; refusing to reconfigure its server.' >&2; exit 2; }
bash "$script_dir/start-local-services.sh" 9>&-
cd /tmp
limit="$(runuser -u postgres -- psql -p 55432 -Atc 'SHOW max_connections')"
if (( limit < 600 )); then
  active="$(runuser -u postgres -- psql -p 55432 -Atc "SELECT count(*) FROM pg_stat_activity WHERE backend_type='client backend' AND pid<>pg_backend_pid()")"
  if (( active > 0 )); then
    echo 'PostgreSQL has active clients; close them before raising max_connections and restarting.' >&2
    exit 2
  fi
  pg_conftool 14 main set max_connections 600
  pg_ctlcluster --skip-systemctl-redirect 14 main stop
  PYTHONPATH="$lab" "$lab/.venv/bin/python" -m scale_lab.memory_control postgres-start 9>&-
fi
