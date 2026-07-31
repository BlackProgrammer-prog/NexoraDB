from __future__ import annotations

import argparse
import os

import uvicorn

from .paths import python_project_dir


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Run the NexoraDB admin API server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--reload", action="store_true")
    args = parser.parse_args(argv)
    run_server(host=args.host, port=args.port, reload=args.reload)


def run_server(*, host: str = "0.0.0.0", port: int = 8000, reload: bool = False) -> None:
    os.chdir(python_project_dir())
    uvicorn.run(
        "nexoradb_admin.app:app",
        host=host,
        port=port,
        reload=reload,
        factory=False,
    )
