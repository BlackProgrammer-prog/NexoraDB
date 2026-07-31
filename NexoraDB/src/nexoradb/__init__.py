from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

from ._version import __version__


def _load_bundled_native() -> None:
    package = sys.modules[__name__]
    native_dir = Path(__file__).resolve().parent / "native"
    native_path = next(native_dir.glob("nexoradb*.so"), None)
    if native_path is None:
        return

    spec = importlib.util.spec_from_file_location(__name__, native_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load bundled NexoraDB engine from {native_path}")

    native_module = importlib.util.module_from_spec(spec)
    sys.modules[__name__] = native_module
    try:
        spec.loader.exec_module(native_module)
    finally:
        sys.modules[__name__] = package

    for name in dir(native_module):
        if name.startswith("__") and name != "__version__":
            continue
        setattr(package, name, getattr(native_module, name))


_load_bundled_native()

__all__ = ["__version__"]
if "DocEngine" in globals():
    __all__.extend(["DocEngine", "GraphManager"])
