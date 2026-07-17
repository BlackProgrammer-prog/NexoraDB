from __future__ import annotations

import argparse
import subprocess
import sys
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

from .paths import dashboard_static_dir, frontend_dir


class _DashboardHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, directory: str | None = None, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def log_message(self, format: str, *args) -> None:  # noqa: A003
        sys.stdout.write("[dashboard] " + format % args + "\n")


def _serve_static(directory: Path, host: str, port: int) -> None:
    if not directory.is_dir():
        raise SystemExit(f"Dashboard directory not found: {directory}")

    handler = lambda *args, **kwargs: _DashboardHandler(  # noqa: E731
        *args,
        directory=str(directory),
        **kwargs,
    )
    server = ThreadingHTTPServer((host, port), handler)
    print(f"Serving NexoraDB dashboard static files from {directory}")
    print(f"Open http://{host}:{port}")
    server.serve_forever()


def _serve_frontend_dev(host: str, port: int) -> None:
    frontend = frontend_dir()
    if not (frontend / "package.json").is_file():
        raise SystemExit(
            "React admin dashboard not found. Expected frontend/package.json in the repository root."
        )

    print(f"Starting Vite dev server for admin dashboard in {frontend}")
    subprocess.run(
        ["npm", "run", "dev", "--", "--host", host, "--port", str(port)],
        cwd=frontend,
        check=True,
    )


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Serve the NexoraDB admin dashboard")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5173)
    parser.add_argument(
        "--mode",
        choices=("dev", "static"),
        default="dev",
        help="dev = Vite React dashboard, static = package dashboard/static folder",
    )
    args = parser.parse_args(argv)

    if args.mode == "static":
        _serve_static(dashboard_static_dir(), args.host, args.port)
    else:
        _serve_frontend_dev(args.host, args.port)


def run_dashboard(*, host: str = "127.0.0.1", port: int = 5173, mode: str = "dev") -> None:
    if mode == "static":
        _serve_static(dashboard_static_dir(), host, port)
    else:
        _serve_frontend_dev(host, port)
