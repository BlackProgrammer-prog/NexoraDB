"""Bounded 50k lifecycle diagnostic. No services, 100M runs or old stores touched."""
import ctypes
import gc
import json
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'nexora-scale-lab'))
import nexoradb as native
from bson import BSON
from scale_lab.memory_diagnostics import python_memory
from scale_lab.resources import process
import os


def main():
    samples = []
    def snapshot(stage, engine=None):
        value = {'stage': stage, 'process': process(os.getpid()), 'allocator': python_memory()}
        if engine is not None:
            value['native'] = json.loads(engine.memory_report())
        samples.append(value)
    with tempfile.TemporaryDirectory(prefix='nexora-memory-lifecycle-') as temporary:
        settings = native.RocksSettings()
        settings.memory_budget_bytes = 64*1024**2
        settings.block_cache_bytes = 32*1024**2
        settings.durability = native.DocumentDurability.Durable
        engine = native.DocEngine(str(Path(temporary) / 'db'), rocks_settings=settings)
        assert engine.create_collection('items').success
        snapshot('opened', engine)
        for start in range(0, 50000, 1000):
            batch = [BSON.encode({'_id': f'{n:08d}', 'v': n, 'payload': 'x'*512})
                     for n in range(start, start+1000)]
            assert engine.insert_many_bson_ingest('items', batch).success
        del batch
        snapshot('loaded', engine)
        for _ in range(32):
            page = engine.find_page('items', limit=1)
            assert page.success
        del page
        snapshot('snapshots_open', engine)
        assert samples[-1]['native']['page_sessions'] == 32
        begin = time.perf_counter()
        assert engine.prepare_shutdown().success
        elapsed = time.perf_counter()-begin
        snapshot('prepared_shutdown', engine)
        assert samples[-1]['native']['page_sessions'] == 0
        del engine
        gc.collect()
        snapshot('engine_destroyed')
        try:
            trim = ctypes.CDLL(None).malloc_trim
            trim.argtypes = [ctypes.c_size_t]
            trim.restype = ctypes.c_int
            trim(0)  # Diagnostic only, after destruction; not a query-path policy.
        except (AttributeError, OSError):
            pass
        snapshot('after_optional_allocator_trim')
    print(json.dumps({'rows': 50000, 'prepare_shutdown_seconds': elapsed,
                      'scope': 'bounded diagnostic, not a full-scale memory stability proof',
                      'samples': samples}, indent=2))


if __name__ == '__main__':
    main()
