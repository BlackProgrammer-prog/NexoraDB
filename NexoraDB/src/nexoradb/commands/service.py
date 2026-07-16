from __future__ import annotations

import os
import signal
import subprocess
import sys
from collections import deque
from pathlib import Path

from .paths import python_project_dir

SERVICE_DIR = Path.home() / ".nexoradb"
PID_FILE = SERVICE_DIR / "service.pid"
LOG_FILE = SERVICE_DIR / "service.log"


def start_service(*, host: str, port: int) -> None:
    SERVICE_DIR.mkdir(parents=True, exist_ok=True)
    existing_pid = _read_pid()
    if existing_pid and _is_running(existing_pid):
        print(f"NexoraDB service is already running with PID {existing_pid}.")
        print(f"Logs: {LOG_FILE}")
        return

    command = [
        sys.executable,
        "-m",
        "uvicorn",
        "nexoradb_admin.app:app",
        "--host",
        host,
        "--port",
        str(port),
    ]
    with LOG_FILE.open("ab") as log:
        process = subprocess.Popen(
            command,
            cwd=python_project_dir(),
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            close_fds=True,
        )
    PID_FILE.write_text(str(process.pid), encoding="utf-8")
    print(f"NexoraDB service started with PID {process.pid}.")
    print(f"Backend: http://{host}:{port}")
    print(f"Logs: {LOG_FILE}")


def stop_service() -> None:
    pid = _read_pid()
    if not pid:
        print("NexoraDB service is not running.")
        return
    if not _is_running(pid):
        PID_FILE.unlink(missing_ok=True)
        print("NexoraDB service is not running.")
        return

    os.kill(pid, signal.SIGTERM)
    PID_FILE.unlink(missing_ok=True)
    print(f"NexoraDB service stopped PID {pid}.")


def show_logs(*, lines: int = 50) -> None:
    if not LOG_FILE.exists():
        print(f"No NexoraDB service log found at {LOG_FILE}.")
        return
    with LOG_FILE.open("r", encoding="utf-8", errors="replace") as log:
        for line in deque(log, maxlen=max(lines, 0)):
            print(line, end="")


def _read_pid() -> int | None:
    try:
        return int(PID_FILE.read_text(encoding="utf-8").strip())
    except (FileNotFoundError, ValueError):
        return None


def _is_running(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True
