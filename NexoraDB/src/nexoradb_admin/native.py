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
        if hasattr(module, "__path__"):
            return _load_native_into_package(path, module)
        raise RuntimeError("Python module 'nexoradb' is already loaded without DocEngine")

    spec = importlib.util.spec_from_file_location("nexoradb", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load native NexoraDB module from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["nexoradb"] = module
    spec.loader.exec_module(module)
    return module


def _load_native_into_package(path: Path, package: ModuleType) -> ModuleType:
    spec = importlib.util.spec_from_file_location("nexoradb", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load native NexoraDB module from {path}")

    native_module = importlib.util.module_from_spec(spec)
    sys.modules["nexoradb"] = native_module
    try:
        spec.loader.exec_module(native_module)
    finally:
        sys.modules["nexoradb"] = package

    for name in dir(native_module):
        if name in {"__name__", "__package__", "__loader__", "__spec__"}:
            continue
        setattr(package, name, getattr(native_module, name))
    if not hasattr(package, "DocEngine"):
        raise RuntimeError(f"native NexoraDB module loaded from {path} without DocEngine")
    return package


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


def create_graph_manager(settings: AdminApiSettings, engine: Any) -> Any:
    native = load_native_module(settings)
    if not getattr(native, "GRAPH_ENABLED", False):
        raise RuntimeError("NexoraDB graph engine is not enabled in the loaded native module")
    graph_manager = native.GraphManager(engine, str(settings.graph_dir))
    graph_manager.startup()
    return graph_manager
