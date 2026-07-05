from __future__ import annotations

import importlib
import importlib.util
import sys
from pathlib import Path
from types import ModuleType
from typing import Any

from .config import AdminApiSettings


def _candidate_native_paths(settings: AdminApiSettings) -> list[Path]:
    candidates: list[Path] = []
    if settings.native_module_path is not None:
        candidates.append(settings.native_module_path)

    cwd = Path.cwd()
    candidates.extend(cwd.glob("nexoradb*.so"))
    for parent in cwd.parents:
        candidates.extend(parent.glob("nexoradb*.so"))
    return candidates


def _load_native_from_path(path: Path) -> ModuleType:
    if "nexoradb" in sys.modules:
        module = sys.modules["nexoradb"]
        if hasattr(module, "DocEngine"):
            return module
        raise RuntimeError(
            "Python package 'nexoradb' is already loaded, so nexoradb.so cannot be loaded "
            "under the same module name. Start the API before importing the Python package, "
            "or pass an engine into create_app()."
        )

    spec = importlib.util.spec_from_file_location("nexoradb", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load native NexoraDB module from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["nexoradb"] = module
    spec.loader.exec_module(module)
    return module


def load_native_module(settings: AdminApiSettings) -> ModuleType:
    for path in _candidate_native_paths(settings):
        if path.is_file():
            return _load_native_from_path(path)

    module = importlib.import_module("nexoradb")
    if not hasattr(module, "DocEngine"):
        raise RuntimeError(
            "native nexoradb module with DocEngine was not found. Build nexoradb.so and run "
            "uvicorn from the repository root, or set NEXORADB_NATIVE_MODULE_PATH."
        )
    return module


def create_doc_engine(settings: AdminApiSettings) -> Any:
    native = load_native_module(settings)
    engine = native.DocEngine(str(settings.db_path))
    if hasattr(engine, "is_healthy") and not engine.is_healthy():
        raise RuntimeError(f"DocEngine failed to open: {settings.db_path}")
    return engine
