from __future__ import annotations

import argparse
import subprocess
import sys
import threading
import time

from nexoradb.cli.main import main as cli_main

from .dashboard import run_dashboard
from .server import run_server
from .service import show_logs, start_service, stop_service


def _run_server_thread(host: str, port: int) -> None:
    run_server(host=host, port=port, reload=False)


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="nexoradb", description="NexoraDB developer commands")
    subparsers = parser.add_subparsers(dest="command", required=True)

    server_parser = subparsers.add_parser("server", help="Run the FastAPI admin/backend server")
    server_parser.add_argument("--host", default="0.0.0.0")
    server_parser.add_argument("--port", type=int, default=8000)
    server_parser.add_argument("--reload", action="store_true")

    run_parser = subparsers.add_parser("run", help="Run the NexoraDB backend server")
    run_parser.add_argument("--host", default="0.0.0.0")
    run_parser.add_argument("--port", type=int, default=8000)
    run_parser.add_argument("--reload", action="store_true")
    run_parser.add_argument("--service", action="store_true", help="Run backend in the background")
    run_parser.add_argument(
        "--log",
        nargs="?",
        const=50,
        type=int,
        metavar="LINES",
        help="Show recent background service logs, default: 50 lines",
    )
    run_parser.add_argument("--stop", action="store_true", help="Stop the background backend service")

    dashboard_parser = subparsers.add_parser("dashboard", help="Serve the admin dashboard UI")
    dashboard_parser.add_argument("--host", default="127.0.0.1")
    dashboard_parser.add_argument("--port", type=int, default=5173)
    dashboard_parser.add_argument("--mode", choices=("dev", "static"), default="dev")

    cli_parser = subparsers.add_parser("cli", help="Launch the Textual admin CLI")
    cli_parser.add_argument("--url", default="http://localhost:8000")

    dev_parser = subparsers.add_parser("dev", help="Run backend and React dashboard together")
    dev_parser.add_argument("--api-host", default="0.0.0.0")
    dev_parser.add_argument("--api-port", type=int, default=8000)
    dev_parser.add_argument("--dashboard-host", default="127.0.0.1")
    dev_parser.add_argument("--dashboard-port", type=int, default=5173)

    args = parser.parse_args(argv)

    if args.command == "server":
        run_server(host=args.host, port=args.port, reload=args.reload)
        return

    if args.command == "run":
        if args.log is not None:
            show_logs(lines=args.log)
            return
        if args.stop:
            stop_service()
            return
        if args.service:
            if args.reload:
                parser.error("nexoradb run --service does not support --reload")
            start_service(host=args.host, port=args.port)
            return
        run_server(host=args.host, port=args.port, reload=args.reload)
        return

    if args.command == "dashboard":
        run_dashboard(host=args.host, port=args.port, mode=args.mode)
        return

    if args.command == "cli":
        sys.argv = ["nexoradb-cli", "--url", args.url]
        cli_main()
        return

    if args.command == "dev":
        server_thread = threading.Thread(
            target=_run_server_thread,
            args=(args.api_host, args.api_port),
            daemon=True,
        )
        server_thread.start()
        time.sleep(1.5)
        print(f"Backend running at http://{args.api_host}:{args.api_port}")
        run_dashboard(host=args.dashboard_host, port=args.dashboard_port, mode="dev")
        return

    parser.print_help()
