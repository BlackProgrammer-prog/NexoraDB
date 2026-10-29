"""Load an explicitly selected experimental extension in a fresh process only."""
import importlib.util
import importlib.metadata
import os
from pathlib import Path
import sys


def runtime_info():
    packages = {}
    for name in ('pymongo', 'lark', 'fastapi', 'httpx', 'uvicorn', 'anyio'):
        try:
            packages[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            packages[name] = None
    return {'python': sys.version, 'executable': str(Path(sys.executable).resolve()), 'packages': packages}


def select_native():
    target = os.environ.get('NEXORA_RESEARCH_NATIVE_PATH')
    if target is None:
        return
    if 'nexoradb' in sys.modules:
        raise RuntimeError('Select the experimental native binary before importing nexoradb')
    path = Path(target).resolve(strict=True)
    if not path.name.startswith('nexoradb') or path.suffix != '.so':
        raise ValueError('Explicit nexoradb .so required')
    spec = importlib.util.spec_from_file_location('nexoradb', path)
    if spec is None or spec.loader is None:
        raise ValueError('Cannot load selected native module')
    module = importlib.util.module_from_spec(spec)
    # The same Python/parser harness is held fixed across native A and B.
    module.__path__ = [str(Path(__file__).resolve().parents[1] / 'NexoraDB/src/nexoradb')]
    module.__native_module_path__ = str(path)
    sys.modules['nexoradb'] = module
    spec.loader.exec_module(module)
