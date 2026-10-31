"""MEM-001/002: repeated private load/query/close/destroy, paired processes with diagnostic trim."""
import argparse
import ctypes
import gc
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time
import tracemalloc

ROOT = Path(__file__).resolve().parents[1]
TELEMETRY = ROOT / 'NexoraDB/src/nexoradb/memory.py'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def slope(values):
    if len(values) < 2 or any(value is None for value in values):
        return None
    center = (len(values) - 1) / 2
    return sum((i - center) * value for i, value in enumerate(values)) / sum((i - center) ** 2 for i in range(len(values)))


def diagnostic_trim(enabled, allocator):
    if not enabled or allocator['provider'] != 'glibc.mallinfo2' or allocator.get('preload'):
        return {'attempted': False, 'reason': 'control_or_allocator_unverified', 'seconds': 0}
    try:
        trim = ctypes.CDLL(None).malloc_trim
        trim.argtypes, trim.restype = [ctypes.c_size_t], ctypes.c_int
    except (AttributeError, OSError):
        return {'attempted': False, 'reason': 'unsupported', 'seconds': 0}
    started = time.perf_counter()
    returned = trim(0)
    return {'attempted': True, 'returned': returned, 'seconds': time.perf_counter() - started,
            'scope': 'diagnostic after destruction and explicit GC only'}


def checked(result):
    if not result.success:
        raise RuntimeError(result.error_msg)


def worker(args):
    native = load('nexoradb', args.native)
    memory = load('nexora_memory_telemetry', TELEMETRY)
    from bson import BSON
    settings = native.RocksSettings()
    settings.memory_budget_bytes, settings.block_cache_bytes = 64 * 1024**2, 32 * 1024**2
    settings.durability = native.DocumentDurability.Durable
    tracemalloc.start(1)  # Diagnostic opt-in, after imports; same overhead in both arms.
    maxima = {'rss_bytes': 0, 'pss_bytes': 0}
    endpoint_path = args.directory / 'endpoints.jsonl'
    with (args.directory / 'samples.jsonl').open('x') as samples, endpoint_path.open('x') as endpoint_output:
        def capture(cycle, stage, engine=None, manager=None):
            sample = memory.collect_memory([engine] if engine is not None else (),
                                           [manager] if manager is not None else (), native=native)
            sample.update(cycle=cycle, stage=stage)
            for key in maxima:
                if sample['process'][key] is not None:
                    maxima[key] = max(maxima[key], sample['process'][key])
            samples.write(json.dumps(sample) + '\n'); samples.flush()
            allocator = sample['allocator']
            allocated = (allocator['allocated_arena_bytes'] + allocator['mmap_bytes']
                         if allocator['allocated_arena_bytes'] is not None else None)
            pmr = dict(native.graph_snapshot_memory())
            return {'rss_bytes': sample['process']['rss_bytes'], 'pss_bytes': sample['process']['pss_bytes'],
                    'allocator_allocated_estimate_bytes': allocated, 'free_arena_bytes': allocator['free_arena_bytes'],
                    'python_traced_current_bytes': sample['python']['traced_current_bytes'],
                    'tracer_overhead_bytes': sample['python']['tracer_overhead_bytes'],
                    'python_allocated_blocks': sample['python']['allocated_blocks'],
                    'snapshot_pmr_bytes': pmr['current_requested_bytes'], 'snapshot_pmr_resources': pmr['live_resources'],
                    'allocator': allocator}

        baseline = capture(-1, 'baseline')
        for cycle in range(args.cycles):
            started = time.perf_counter()
            with tempfile.TemporaryDirectory(prefix='owned-cycle-', dir=args.directory) as private:
                engine = native.DocEngine(str(Path(private) / 'documents'), rocks_settings=settings)
                engine.pause_document_migration(); checked(engine.create_collection('items'))
                index = native.IndexDefinition(); index.index_name = 'key'; index.fields = ['key']
                checked(engine.create_index('items', index)); capture(cycle, 'opened', engine)
                for start in range(0, args.rows, 500):
                    batch = [BSON.encode({'_id': f'{n:08d}', 'key': str(n), 'next': f'{(n + 1) % args.rows:08d}',
                        'payload': (hashlib.sha256(str(n).encode()).hexdigest() * ((args.payload_bytes + 63) // 64))[:args.payload_bytes]})
                        for n in range(start, min(start + 500, args.rows))]
                    checked(engine.insert_many_bson_ingest('items', batch))
                del batch
                capture(cycle, 'loaded', engine)
                if args.flush_before_query:
                    checked(engine.flush_storage())  # Explicit workload action, never a telemetry side effect.
                    capture(cycle, 'sst_flushed', engine)
                manager = native.GraphManager(engine, str(Path(private) / 'graphs'))
                definition = native.GraphDefinition(); definition.name = 'ring'
                node = native.NodeMappingDef(); node.node_type = 'Item'; node.collection = 'items'; node.key_path = '_id'
                edge = native.EdgeMappingDef(); edge.edge_type = 'Next'; edge.collection = 'items'
                edge.source_path = '_id'; edge.target_path = 'next'; edge.source_node_type = edge.target_node_type = 'Item'
                definition.node_mappings.append(node); definition.edge_mappings.append(edge)
                if not manager.create_graph(definition):
                    raise RuntimeError('create graph failed')
                built = manager.build_graph('ring'); checked(built)
                for n in range(20):
                    condition = native.Condition.leaf('key', native.Op.EQ, str(n % args.rows))
                    checked(engine.find_many('items', condition, 1))
                scan = native.Condition.leaf('key', native.Op.GTE, '') if args.flush_before_query else native.Condition()
                result = engine.find_many('items', scan, 0); checked(result)
                result_json = result.data; decoded = json.loads(result_json)
                if len(decoded) != args.rows:
                    raise RuntimeError('query count mismatch')
                capture(cycle, 'query_results_held', engine, manager)
                del result, result_json, decoded
                tokens = []
                for _ in range(8):
                    page = engine.find_page('items', limit=1)
                    if not page.success:
                        raise RuntimeError(page.error_msg)
                    tokens.append(page.continuation_token)
                del page
                options = native.SnapshotOptions(); options.use_cache = True
                snapshots = [manager.create_snapshot('ring', options=options) for _ in range(2)]
                for snapshot in snapshots:
                    if snapshot.node_count() != args.rows or snapshot.edge_count() != args.rows:
                        raise RuntimeError('snapshot topology mismatch')
                del snapshot
                capture(cycle, 'snapshots_held', engine, manager)
                for token in tokens:
                    engine.close_page(token)
                del tokens
                state = json.loads(engine.memory_report())
                if state['page_live_snapshots'] != 0:
                    raise RuntimeError('page snapshot survived close')
                del state
                capture(cycle, 'cursors_closed', engine, manager)
                if not manager.prepare_shutdown():
                    raise RuntimeError('graph prepare shutdown failed')
                manager.shutdown(); capture(cycle, 'graph_closed', engine, manager)
                checked(engine.prepare_shutdown()); capture(cycle, 'document_closed', engine, manager)
                del snapshots, manager, engine, built, definition, node, edge, index, options, condition, token, scan
                collected = gc.collect()  # Explicit experiment step; collect_memory never calls GC.
                before = capture(cycle, 'destroyed_before_trim')
                if (before['snapshot_pmr_bytes'], before['snapshot_pmr_resources']) != (baseline['snapshot_pmr_bytes'], baseline['snapshot_pmr_resources']):
                    raise RuntimeError('snapshot PMR survived destruction')
                trim = diagnostic_trim(args.mode == 'trim', before['allocator'])
                after = capture(cycle, 'after_diagnostic_trim' if args.mode == 'trim' else 'control_after_observation')
                for point in (before, after):
                    del point['allocator']
                endpoint = {'cycle': cycle, 'warmup': cycle < args.warmup, 'before': before, 'after': after,
                    'trim': trim, 'gc_collected_objects': collected, 'cycle_seconds': time.perf_counter() - started}
                endpoint_output.write(json.dumps(endpoint) + '\n'); endpoint_output.flush()
                print(f'{args.mode} cycle {cycle + 1}/{args.cycles}: rss={after["rss_bytes"]}', flush=True)
                del endpoint, before, after, point, trim
    # Analysis begins after the last process sample: retained summaries cannot pin
    # pymalloc arenas during the experiment and impersonate engine retention.
    endpoints = [json.loads(line) for line in endpoint_path.read_text().splitlines()]
    measured = endpoints[args.warmup:]
    statistics = {}
    for phase in ('before', 'after'):
        statistics[phase] = {}
        for key in ('rss_bytes', 'pss_bytes', 'allocator_allocated_estimate_bytes', 'free_arena_bytes', 'python_traced_current_bytes', 'tracer_overhead_bytes'):
            values = [point[phase][key] for point in measured]; present = [v for v in values if v is not None]
            statistics[phase][key] = {'slope_bytes_per_cycle': slope(values),
                'first_to_last_bytes': values[-1] - values[0] if present and len(present) == len(values) else None,
                'observed_min_bytes': min(present) if present else None, 'observed_max_bytes': max(present) if present else None}
    result = {'status': 'passed', 'mode': args.mode, 'baseline': baseline, 'cycles': endpoints,
              'warmup_cycles_excluded': args.warmup, 'statistics': statistics, 'sampled_peaks': maxima,
              'interpretation': 'bounded observations, not a leak detector or proven bound; PMR release verified',
              'observer': 'tracemalloc enabled in both arms; full samples and endpoints streamed; summary analysis runs after last sample'}
    (args.directory / 'summary.json').write_text(json.dumps(result, indent=2))


def campaign(args):
    args.directory.mkdir(parents=True, exist_ok=False)
    (args.directory / 'runner.py').write_bytes(Path(__file__).read_bytes())
    (args.directory / 'telemetry.py').write_bytes(TELEMETRY.read_bytes())
    manifest = {'schema_version': 1, 'status': 'running', 'rows': args.rows, 'payload_bytes': args.payload_bytes,
        'cycles': args.cycles, 'warmup': args.warmup, 'repetitions': args.repetitions,
        'storage_phase': 'flushed_sst_warmed_cache' if args.flush_before_query else 'memtable',
        'native': str(args.native), 'native_sha256': hashlib.sha256(args.native.read_bytes()).hexdigest(),
        'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'telemetry_sha256': hashlib.sha256(TELEMETRY.read_bytes()).hexdigest(),
        'python': sys.version, 'platform': platform.platform(), 'libc': platform.libc_ver(),
        'allocator_environment': {key: os.getenv(key) for key in ('LD_PRELOAD', 'MALLOC_ARENA_MAX', 'MALLOC_TRIM_THRESHOLD_')}, 'runs': []}
    try:
        for repetition in range(args.repetitions):
            for mode in (('control', 'trim') if repetition % 2 == 0 else ('trim', 'control')):
                directory = args.directory / f'{repetition}-{mode}'; directory.mkdir()
                with (directory / 'stdout.log').open('x') as stdout, (directory / 'stderr.log').open('x') as stderr:
                    command = [sys.executable, str(Path(__file__).resolve()), '--worker', '--directory', str(directory),
                        '--native', str(args.native), '--mode', mode, '--rows', str(args.rows), '--cycles', str(args.cycles),
                        '--warmup', str(args.warmup), '--payload-bytes', str(args.payload_bytes)]
                    if args.flush_before_query:
                        command.append('--flush-before-query')
                    result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=600)
                if result.returncode:
                    raise RuntimeError('worker failed: ' + str(directory))
                summary = json.loads((directory / 'summary.json').read_text())
                manifest['runs'].append({'repetition': repetition, 'mode': mode, 'summary': str(directory / 'summary.json'),
                    'statistics': summary['statistics'], 'sampled_peaks': summary['sampled_peaks']})
                print(f'{repetition}-{mode}: lifecycle and snapshot release verified', flush=True)
        manifest['status'] = 'passed'
    except BaseException as error:
        manifest.update(status='failed', error=str(error)); raise
    finally:
        (args.directory / 'report.json').write_text(json.dumps(manifest, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--native', type=Path, default=ROOT / 'build/debug/nexoradb.cpython-310-x86_64-linux-gnu.so')
    parser.add_argument('--rows', type=int, choices=range(100, 50001), default=12000, metavar='100..50000')
    parser.add_argument('--payload-bytes', type=int, choices=range(64, 2049), default=512, metavar='64..2048')
    parser.add_argument('--cycles', type=int, choices=range(4, 21), default=8)
    parser.add_argument('--warmup', type=int, default=2)
    parser.add_argument('--repetitions', type=int, choices=range(1, 5), default=3)
    parser.add_argument('--flush-before-query', action='store_true', help='explicit flush and indexed range scan to exercise SST cache')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--mode', choices=('control', 'trim'), default='control', help=argparse.SUPPRESS)
    args = parser.parse_args(); args.native = args.native.resolve(strict=True); args.directory = args.directory.resolve()
    if not 0 <= args.warmup <= args.cycles - 3:
        parser.error('need at least three measured cycles after warmup')
    worker(args) if args.worker else campaign(args)


if __name__ == '__main__':
    main()
