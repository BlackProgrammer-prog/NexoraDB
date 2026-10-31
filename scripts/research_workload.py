#!/usr/bin/env python3
"""Small native correctness driver; not a publication or RAM-limit certification."""
import hashlib
import argparse
import json
import math
import os
from pathlib import Path
import resource
from concurrent.futures import ThreadPoolExecutor
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))
sys.path.insert(0, str(ROOT / 'nexora-scale-lab'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from research_ab import json_hash, digest
from scale_lab.dataset import DatasetConfig, batches


def edges(index, count):
    return {(index + step) % count for step in (1, 7, 13)}


def reachable(index, count):
    visited, frontier = {index}, {index}
    distances = {}
    for distance in (1, 2):
        next_frontier = set()
        for node in frontier:
            for other in edges(node, count) - visited:
                distances[other] = distance
                next_frontier.add(other)
        visited.update(next_frontier)
        frontier = next_frontier
    return distances


def execute(workload):
    if 'NEXORA_RESEARCH_CPUS' in os.environ:
        os.sched_setaffinity(0, json.loads(os.environ['NEXORA_RESEARCH_CPUS']))
    from research_native import select_native, runtime_info
    select_native()
    from scale_lab.adapters import NexoraAdapter, native_result
    import nexoradb
    if workload.get('version') not in (2, 3):
        raise ValueError('This driver implements trace versions 2/3; old results are never relabeled')
    scenario = workload['scenario']
    count, operations = workload['records'], workload['operations']
    if scenario not in {'document-oltp', 'document-graph', 'low-memory'}:
        raise ValueError('Unknown scenario')
    allow_long = workload.get('allow_long', False)
    if type(allow_long) is not bool:
        raise ValueError('allow_long must be boolean')
    maximum = 2_000_000 if allow_long else 10000
    if type(count) is not int or not 32 <= count <= 10000 or type(operations) is not int or not 10 <= operations <= maximum:
        raise ValueError('Records must be 32..10000; >10000 operations needs explicit allow_long=true')
    clients = workload['clients']
    if type(clients) is not int or clients not in (1, 10) or count < clients * 3 or operations % (2 * clients):
        raise ValueError('Clients must be 1 or 10, >=3 records/client, operations divisible by 2*clients')
    if workload['durability'] != 'local-wal-sync' or workload['transport'] != 'embedded':
        raise ValueError('Unsupported durability/transport')
    if workload['read_percent'] != (50 if scenario == 'document-oltp' else 100):
        raise ValueError('Read mix does not match scenario')
    config = DatasetConfig(count, workload['seed'], 100)
    gm = executor = adapter = None
    executors = []
    local = None
    timings = []
    with tempfile.TemporaryDirectory(prefix='nexora-research-') as directory:
        try:
            adapter = NexoraAdapter(directory + '/db', memory_budget_bytes=(32 if scenario == 'low-memory' else 128) * 1024**2)
            adapter.connect()
            adapter.prepare_schema()
            adapter.create_indexes()
            expected = {value['id']: value for batch in batches(config) for value in batch}
            for batch in batches(config):
                adapter.batch_insert(batch)
            if scenario == 'document-graph':
                from nexoradb.parser import Executor
                gm = nexoradb.GraphManager(adapter.engine, directory + '/graphs')
                if not gm.startup():
                    raise ValueError('Graph startup failed')
                executor = Executor(adapter.engine, gm)
                native_result(adapter.engine.create_collection('research_edges'))
                for source in range(count):
                    for target in edges(source, count):
                        native_result(adapter.engine.insert_one('research_edges', json.dumps({
                            '_id': f'e{source}_{target}', 'src': f'{source:012d}', 'dst': f'{target:012d}'})))
                for statement in ('CREATE LIVE GRAPH research HETEROGENEOUS DIRECTED;',
                                  'USE GRAPH research;',
                                  'MAP NODE User FROM scale_lab_records KEY _id;',
                                  'MAP EDGE LINK FROM research_edges SOURCE src AS User TARGET dst AS User;',
                                  'BUILD GRAPH research;'):
                    if not all(row.get('success') is True for row in executor.execute_text(statement)):
                        raise ValueError('Graph setup failed: ' + statement)

            executors = [executor]
            if executor is not None and clients > 1:
                executors = []
                for _ in range(clients):
                    local = Executor(adapter.engine, gm)
                    if not all(row.get('success') is True for row in local.execute_text('USE GRAPH research;')):
                        raise ValueError('Worker graph selection failed')
                    executors.append(local)

            def action(step, worker):
                # Disjoint key partitions make the multi-client final-state oracle deterministic.
                choices = len(range(worker, count, clients))
                random_key = int.from_bytes(hashlib.sha256(f'{config.seed}:{worker}:{step}'.encode()).digest()[:8], 'big')
                index = worker + clients * (random_key % choices)
                key = f'{index:012d}'
                if scenario == 'document-graph':
                    rows = executors[worker].execute_text(f"TRAVERSE User('{key}') OUT LINK DEPTH 2 LIMIT 1000;")
                    if len(rows) != 1 or rows[0].get('success') is not True:
                        raise ValueError('Traversal failed')
                    actual = {int(node['id']): node['distance'] for node in rows[0]['nodes']}
                    if actual != reachable(index, count) or len(actual) != rows[0]['count']:
                        raise ValueError('Traversal differs from independent set/BFS oracle')
                    for neighbor in actual:
                        if adapter.read_by_id(f'{neighbor:012d}') != expected[f'{neighbor:012d}']:
                            raise ValueError('Graph/document hydration mismatch')
                elif scenario == 'document-oltp' and step % 2:
                    value = 'active' if step % 4 == 1 else 'inactive'
                    adapter.update(key, value)
                    expected[key] = {**expected[key], 'status': value}
                else:
                    found = adapter.read_by_field('username', f'user_{index}')
                    if found != [expected[key]]:
                        raise ValueError('Indexed lookup mismatch')

            # Warm-up uses its own deterministic trace; writes update the oracle.
            warmup = workload.get('warmup_operations', 10 * clients)
            if type(warmup) is not int or warmup < 0 or warmup > 100000 or warmup % clients:
                raise ValueError('warmup_operations must be 0..100000 and divisible by clients')
            def measure_worker(worker):
                samples = []
                last_log = time.monotonic()
                def log_progress(phase, completed, total):
                    nonlocal last_log
                    if allow_long and time.monotonic() - last_log >= 5:
                        print(json.dumps({'event': 'workload-progress', 'phase': phase, 'worker': worker,
                                          'completed': completed, 'total': total}), file=sys.stderr, flush=True)
                        last_log = time.monotonic()
                try:
                    for step in range(warmup // clients):
                        action(step, worker)
                        if step % 1000 == 0:
                            log_progress('warming', step, warmup // clients)
                    ready.wait(timeout=300 if allow_long else 30)
                    start_measurement.wait(timeout=10)
                    for step in range(warmup // clients, (warmup + operations) // clients):
                        started = time.perf_counter_ns()
                        action(step, worker)
                        samples.append(time.perf_counter_ns() - started)
                        if step % 1000 == 0:
                            log_progress('measuring', len(samples), operations // clients)
                    return samples, threading.get_ident()
                except BaseException:
                    ready.abort()
                    start_measurement.abort()
                    raise
            ready = threading.Barrier(clients + 1)
            start_measurement = threading.Barrier(clients + 1)
            worker_ids = set()
            with ThreadPoolExecutor(max_workers=clients) as pool:
                futures = [pool.submit(measure_worker, worker) for worker in range(clients)]
                ready.wait(timeout=300 if allow_long else 30)
                measurement_started = time.perf_counter_ns()
                start_measurement.wait(timeout=10)
                for future in futures:
                    values, worker_id = future.result()
                    timings.extend(values)
                    worker_ids.add(worker_id)
                measurement_ns = time.perf_counter_ns() - measurement_started
            if len(worker_ids) != clients:
                raise ValueError('Requested concurrent workers were not established')
            # Full post-state check, excluded from measured operation time.
            actual = {value['id']: value for value in adapter.iter_records()}
            if actual != expected or adapter.count() != count:
                raise ValueError('Full document state mismatch')
            report = {'correct': True, 'errors': 0, 'operations': operations,
                      'workload_sha256': json_hash(workload),
                      'ops_s': operations * 1e9 / measurement_ns,
                      'p99_us': sorted(timings)[math.ceil(.99 * len(timings)) - 1] / 1000,
                      'resource_scope': 'engine+Python driver+oracle in one process; do not add them again',
                      'max_rss_including_setup_and_oracle_bytes': resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024,
                      'native_build': nexoradb.build_info(),
                      'driver_runtime': runtime_info(),
                      'loaded_artifacts': {str(Path(nexoradb.__native_module_path__).resolve()): digest(nexoradb.__native_module_path__)},
                      'clients': clients,
                      'warmup_operations': warmup,
                      'effective_affinity': sorted(os.sched_getaffinity(0)),
                      'concurrent_worker_threads': len(worker_ids),
                      'cgroup_membership': Path('/proc/self/cgroup').read_text(),
                      'oracle_in_timed_path': True, 'memory_limit_enforced': False,
                      'publication_eligible': False}
        finally:
            closing = time.perf_counter()
            for active in executors or [executor]:
                if active is not None:
                    active.abort_transaction()
            active = None
            if gm is not None:
                gm.shutdown()
            # Release every Python keep-alive before timing DocEngine destruction.
            executors.clear()
            local = executor = gm = None
            if adapter is not None:
                adapter.disconnect()
                if 'report' in locals():
                    report['close_seconds'] = time.perf_counter() - closing
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=['document-oltp', 'document-graph', 'low-memory'])
    parser.add_argument('--clients', type=int, choices=[1, 10], default=1)
    parser.add_argument('--records', type=int, default=64)
    parser.add_argument('--operations', type=int, default=100)
    args = parser.parse_args()
    if args.scenario:
        workload = {'version': 2, 'scenario': args.scenario, 'seed': 2026,
                    'records': args.records, 'operations': args.operations, 'clients': args.clients,
                    'read_percent': 50 if args.scenario == 'document-oltp' else 100,
                    'durability': 'local-wal-sync', 'transport': 'embedded'}
    else:
        workload = json.loads(os.environ['NEXORA_RESEARCH_WORKLOAD'])
    # Native graph logs also use fd 1. Keep stdout a strict JSON driver protocol.
    sys.stdout.flush()
    saved = os.dup(1)
    try:
        os.dup2(2, 1)
        result = execute(workload)
    finally:
        sys.stdout.flush()
        os.dup2(saved, 1)
        os.close(saved)
    print(json.dumps(result, allow_nan=False))


if __name__ == '__main__':
    main()
