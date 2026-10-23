#!/usr/bin/env bash
# Apply only to the dedicated local 14/main instance; never while a trial runs.
set -euo pipefail
[[ "$(id -u)" == 0 ]] || { echo 'Run as root inside Ubuntu WSL' >&2; exit 2; }
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd -- "$script_dir/../.." && pwd)"
mkdir -p "$lab/state"
exec 9>>"$lab/state/protocol.lock"
flock -n 9 || { echo 'Benchmark active; refusing server changes' >&2; exit 2; }
if pg_ctlcluster 14 main status >/dev/null 2>&1; then
  cd /tmp
  active="$(runuser -u postgres -- psql -p 55432 -Atc "SELECT count(*) FROM pg_stat_activity WHERE backend_type='client backend' AND pid<>pg_backend_pid()")"
  (( active == 0 )) || { echo 'PostgreSQL has active clients; not restarting' >&2; exit 2; }
  pg_ctlcluster --skip-systemctl-redirect 14 main stop
fi
install -m 644 "$script_dir/postgres-scale-lab.conf" /etc/postgresql/14/main/conf.d/90-scale-lab.conf
export PYTHONPATH="$lab/..:$lab${PYTHONPATH:+:$PYTHONPATH}"
"$lab/.venv/bin/python" -m scale_lab.memory_control prepare
"$lab/.venv/bin/python" -m scale_lab.memory_control postgres-start 9>&-
cd /tmp
runuser -u postgres -- psql -p 55432 -Atc 'SHOW shared_buffers; SHOW max_wal_size; SHOW fsync; SHOW full_page_writes; SHOW synchronous_commit;'
