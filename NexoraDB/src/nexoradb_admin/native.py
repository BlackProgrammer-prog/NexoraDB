from __future__ import annotations

import importlib
import importlib.util
import hashlib
import logging
import json
import sys
from pathlib import Path
from types import ModuleType
from typing import Any
from threading import Lock

from .config import AdminApiSettings

_EXECUTION_POOLS = {}
_EXECUTION_POOL_LOCK = Lock()


def _execution_pool(native, capacity):
    with _EXECUTION_POOL_LOCK:
        key = (native.ResourceBudget, capacity)
        if key not in _EXECUTION_POOLS:
            _EXECUTION_POOLS[key] = native.ResourceBudget(capacity)
        return _EXECUTION_POOLS[key]


def _candidate_native_paths(settings: AdminApiSettings) -> list[Path]:
    candidates: list[Path] = []
    if settings.native_module_path is not None:
        candidates.append(settings.native_module_path)

    package_native_dir = Path(__file__).resolve().parent.parent / "nexoradb" / "native"
    candidates.extend(package_native_dir.glob("nexoradb*.so"))

    cwd = Path.cwd()
    candidates.extend(cwd.glob("nexoradb*.so"))
    for parent in cwd.parents:
        candidates.extend(parent.glob("nexoradb*.so"))
    return candidates


def _load_native_from_path(path: Path) -> ModuleType:
    if "nexoradb" in sys.modules:
        module = sys.modules["nexoradb"]
        if hasattr(module, "DocEngine"):
            loaded = getattr(module, '__native_module_path__', getattr(module, '__file__', None))
            if loaded and Path(loaded).suffix == '.so' and Path(loaded).resolve() != path.resolve():
                def digest(binary):
                    checksum = hashlib.sha256()
                    with Path(binary).open('rb') as source:
                        for block in iter(lambda: source.read(1024*1024), b''):
                            checksum.update(block)
                    return checksum.digest()
                if digest(loaded) != digest(path):
                    raise RuntimeError('A different native engine is already loaded. Synchronize the packaged Release binary and restart; hot swapping native types is unsafe.')
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
    package.__native_module_path__ = str(path.resolve())
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
    if not hasattr(native.DocEngine, 'durability_report'):
        raise RuntimeError('Native durability reporting missing; rebuild current extension before opening storage')
    rocks = native.RocksSettings()
    if type(settings.rocks_durability) is not str or settings.rocks_durability not in {'durable', 'buffered'}:
        raise RuntimeError('NEXORADB_ROCKS_DURABILITY must be durable or buffered')
    rocks.durability = (native.DocumentDurability.Durable if settings.rocks_durability == 'durable'
                        else native.DocumentDurability.Buffered)
    rocks.memory_budget_bytes = settings.rocks_memory_mb * 1024**2
    rocks.block_cache_bytes = settings.rocks_cache_mb * 1024**2
    if settings.rocks_auto_tune:
        from .hardware import detect, recommend
        observed = detect(settings.db_path)
        effective = recommend(observed, settings.rocks_memory_mb, settings.rocks_cache_mb)
        rocks.memory_budget_bytes = effective['memory_budget_bytes']
        rocks.block_cache_bytes = effective['block_cache_bytes']
        rocks.cpu_threads = effective['cpu_threads']
        rocks.max_background_jobs = effective['max_background_jobs']
        rocks.disk = {'ssd': native.DiskClass.SSD, 'rotational': native.DiskClass.Rotational,
                      'unknown': native.DiskClass.Unknown}[effective['disk_class']]
        logging.getLogger(__name__).info('Hardware policy observed=%s effective=%s', observed, effective)
    if settings.execution_memory_mb or settings.resource_process_mb:
        if not hasattr(native, 'ResourceBudget'):
            raise RuntimeError('Shared resource admission missing; rebuild native module')
        requested = settings.execution_memory_mb * 1024**2
        if settings.resource_process_mb:
            spec = importlib.util.spec_from_file_location('_nexora_resource_sizing', Path(__file__).resolve().parent.parent / 'nexoradb/execution.py')
            sizing = importlib.util.module_from_spec(spec); spec.loader.exec_module(sizing)
            plan = sizing.size_execution_budget(process_bytes=settings.resource_process_mb * 1024**2,
                rocks_bytes=rocks.memory_budget_bytes, allocator_bytes=settings.resource_allocator_margin_mb * 1024**2,
                python_bytes=settings.resource_python_margin_mb * 1024**2,
                page_cache_bytes=settings.resource_page_cache_margin_mb * 1024**2,
                safety_bytes=settings.resource_safety_margin_mb * 1024**2)
            if requested and requested > plan['execution_bytes']:
                raise RuntimeError('execution reservation capacity exceeds configured process-envelope headroom')
            requested = requested or plan['execution_bytes']
            logging.getLogger(__name__).info('Resource admission sizing=%s', plan)
        rocks.execution_budget = _execution_pool(native, requested)
    engine = native.DocEngine(str(settings.db_path), native.TransactionSettings(), rocks)
    if not hasattr(engine, 'durability_report'):
        raise RuntimeError('Native durability reporting missing; rebuild current extension')
    report = json.loads(engine.durability_report())
    if (report.get('profile') != settings.rocks_durability or report.get('wal_enabled') is not True
            or report.get('sync_on_ack') != (settings.rocks_durability == 'durable')
            or report.get('manual_wal_flush') is not False):
        raise RuntimeError('Effective native durability does not match requested profile')
    logging.getLogger(__name__).info('Effective document durability=%s', report)
    if hasattr(engine, "is_healthy") and not engine.is_healthy():
        raise RuntimeError(f"DocEngine failed to open: {settings.db_path}")
    return engine


def create_graph_manager(settings: AdminApiSettings, engine: Any) -> Any:
    native = load_native_module(settings)
    if not getattr(native, "GRAPH_ENABLED", False):
        raise RuntimeError("NexoraDB graph engine is not enabled in the loaded native module")
    graph_manager = native.GraphManager(engine, str(settings.graph_dir))
    if not graph_manager.startup():
        graph_manager.shutdown()
        raise RuntimeError("Graph recovery failed; database is not ready")
    return graph_manager
