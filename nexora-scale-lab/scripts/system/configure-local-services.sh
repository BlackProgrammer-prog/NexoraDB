#!/usr/bin/env bash
# One-time configuration for fresh Ubuntu 22.04 package installations.
set -euo pipefail
: "${SCALE_LAB_TEMP_PASSWORD:?Set the temporary benchmark password}"
if [[ "$(id -u)" != 0 ]]; then
  echo 'Run as root inside Ubuntu WSL.' >&2
  exit 1
fi
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pg_conftool 14 main set port 55432
pg_conftool 14 main set listen_addresses localhost
pg_conftool 14 main set fsync on
pg_conftool 14 main set synchronous_commit on
pg_ctlcluster --skip-systemctl-redirect 14 main restart
runuser -u postgres -- psql --port 55432 --set "benchmark_password=$SCALE_LAB_TEMP_PASSWORD" \
  --file - < "$script_dir/create-postgres.sql"
install -m 644 "$script_dir/mongod-scale-lab.conf" /etc/mongod.conf
bash "$script_dir/start-local-services.sh"
