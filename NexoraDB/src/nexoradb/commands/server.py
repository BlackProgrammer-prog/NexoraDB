from __future__ import annotations

import argparse
import os

import uvicorn

from .paths import python_project_dir


class ControlledServer(uvicorn.Server):
    """Signal requests cooperative cancellation before Uvicorn starts draining."""
    def handle_exit(self, sig, frame):
        application = getattr(self.config, "loaded_app", None)
        seen = set()
        while application is not None and id(application) not in seen:
            seen.add(id(application))
            lifecycle = getattr(application, "nexora_lifecycle", None)
            if lifecycle is None:
                lifecycle = getattr(getattr(application, "state", None), "lifecycle", None)
            if lifecycle is not None:
                lifecycle.stop_admission("signal:"+str(sig))
                break
            application = getattr(application, "app", None)
        self.should_exit = True  # Repeated signals do not abandon native workers.


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Run the NexoraDB admin API server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--reload", action="store_true")
    args = parser.parse_args(argv)
    run_server(host=args.host, port=args.port, reload=args.reload)


def run_server(*, host: str = "127.0.0.1", port: int = 8000, reload: bool = False) -> None:
    os.chdir(python_project_dir())
    if reload:
        uvicorn.run("nexoradb_admin.app:app", host=host, port=port, reload=True, factory=False)
    else:
        ControlledServer(uvicorn.Config("nexoradb_admin.app:app", host=host, port=port,
                                       factory=False)).run()
