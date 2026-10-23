#!/usr/bin/env bash
# Run as root inside Ubuntu WSL after package installation/configuration.
set -euo pipefail
if [[ "$(id -u)" != 0 ]]; then
  echo 'Run this script as root inside the Ubuntu WSL distribution.' >&2
  exit 1
fi
if ! pg_ctlcluster 14 main status >/dev/null 2>&1; then
  script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  lab="$(cd -- "$script_dir/../.." && pwd)"
  PYTHONPATH="$lab${PYTHONPATH:+:$PYTHONPATH}" "$lab/.venv/bin/python" -m scale_lab.memory_control postgres-start
fi
if ! pgrep -u mongodb -x mongod >/dev/null; then
  runuser -u mongodb -- mongod --config /etc/mongod.conf --fork
fi
