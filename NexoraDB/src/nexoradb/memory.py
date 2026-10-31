"""Low-frequency ownership telemetry. Reading counters never collects GC or trims malloc.

Requested/capacity estimates are not resident pages. No sum in this module is a
process memory total. See docs/cpp/MEMORY_OWNERSHIP_FA.md for coverage and gaps.
"""
from __future__ import annotations

import ctypes
import os
from pathlib import Path
import platform
import sys
import time
import tracemalloc
import json


def process_memory(proc_root=Path('/proc')):
    """One smaps_rollup read gives a consistent Linux RSS/PSS partition, or nulls."""
    fields = {'Rss': 'rss_bytes', 'Pss': 'pss_bytes', 'Pss_Anon': 'pss_anon_bytes',
              'Pss_File': 'pss_file_bytes', 'Pss_Shmem': 'pss_shmem_bytes',
              'Private_Clean': 'private_clean_bytes', 'Private_Dirty': 'private_dirty_bytes',
              'Shared_Clean': 'shared_clean_bytes', 'Shared_Dirty': 'shared_dirty_bytes',
              'Anonymous': 'anonymous_bytes', 'Swap': 'swap_bytes', 'SwapPss': 'swap_pss_bytes'}
    result = {'pid': os.getpid(), **dict.fromkeys(fields.values()), 'source': 'smaps_rollup', 'errors': []}
    try:
        for line in (Path(proc_root) / 'self/smaps_rollup').read_text().splitlines():
            name, _, rest = line.partition(':')
            if name in fields:
                value, unit = rest.split()
                if unit != 'kB':
                    raise ValueError('unexpected proc memory unit')
                result[fields[name]] = int(value) * 1024
    except (OSError, ValueError) as error:
        result['errors'].append(type(error).__name__)
    partition = [result[name] for name in ('private_clean_bytes', 'private_dirty_bytes',
                                         'shared_clean_bytes', 'shared_dirty_bytes')]
    result['rss_partition_delta_bytes'] = (result['rss_bytes'] - sum(partition)
        if result['rss_bytes'] is not None and all(value is not None for value in partition) else None)
    return result


def allocator_memory():
    """glibc mallinfo2 describes the process allocator, never just Python/native live objects."""
    result = {'provider': None, 'arena_bytes': None, 'allocated_arena_bytes': None,
              'free_arena_bytes': None, 'mmap_bytes': None, 'top_releasable_estimate_bytes': None,
              'error': None}
    if platform.libc_ver()[0] != 'glibc':
        result['error'] = 'glibc mallinfo2 unavailable or allocator ownership unverified'
        return result
    class Mallinfo(ctypes.Structure):
        _fields_ = [(name, ctypes.c_size_t) for name in
                    ('arena', 'ordblks', 'smblks', 'hblks', 'hblkhd', 'usmblks',
                     'fsmblks', 'uordblks', 'fordblks', 'keepcost')]
    try:
        query = ctypes.CDLL(None).mallinfo2
        query.restype, query.argtypes = Mallinfo, []
        value = query()
        result.update(provider='glibc.mallinfo2', arena_bytes=value.arena,
                      allocated_arena_bytes=value.uordblks, free_arena_bytes=value.fordblks,
                      mmap_bytes=value.hblkhd, top_releasable_estimate_bytes=value.keepcost)
    except (AttributeError, OSError) as error:
        result['error'] = type(error).__name__
    # LD_PRELOAD allocators may intercept malloc while glibc still exports mallinfo2.
    result['preload'] = os.environ.get('LD_PRELOAD') or None
    result['coverage'] = 'glibc only; tcache, pymalloc arenas, other allocators and direct mmap are not live-object attribution'
    return result


def python_memory():
    tracing = tracemalloc.is_tracing()
    current, peak = tracemalloc.get_traced_memory() if tracing else (None, None)
    return {'allocated_blocks': sys.getallocatedblocks() if hasattr(sys, 'getallocatedblocks') else None,
            'tracemalloc_enabled': tracing, 'traced_current_bytes': current, 'traced_peak_bytes': peak,
            'tracer_overhead_bytes': tracemalloc.get_tracemalloc_memory() if tracing else None,
            'total_live_bytes': None,
            'coverage': 'traces since tracing enabled, not all Python objects; C++ new/malloc generally untraced; pymalloc overlaps glibc arenas'}


def collect_memory(engines=(), managers=(), *, native=None):
    """Collect one process; repeated engine/manager references and shared RocksResources are deduplicated.

    Pass native for process-wide snapshot counters even after managers are destroyed.
    Native calls may block briefly on ownership locks; use outside latency-critical paths.
    """
    started = time.monotonic_ns()
    os_memory, allocator, python = process_memory(), allocator_memory(), python_memory()
    measurements, contributions, documents, graphs = [], {}, [], []
    def metric(name, value, owner, measurement='estimate', overlaps=(), reason=None, unit='bytes'):
        item = {'id': name, 'value': value, 'owner': owner, 'unit': unit,
                'measurement': 'unavailable' if value is None else measurement,
                'overlaps': list(overlaps), 'reason': reason}
        measurements.append(item)
        return item
    seen_engines, resources = set(), set()
    for engine in engines:
        if id(engine) in seen_engines:
            continue
        seen_engines.add(id(engine))
        raw = json.loads(engine.memory_report())
        if raw.get('schema_version') != 2:
            raise RuntimeError('Memory telemetry requires native schema 2; rebuild native module')
        owner = f'document:{len(documents)}'
        resource = f'rocks_resource:{raw["rocks_resource_id"]}'
        documents.append({'owner': owner, 'raw': raw})
        if resource not in resources:
            resources.add(resource)
            for key in ('cache_usage_bytes', 'cache_pinned_bytes', 'cache_capacity_bytes',
                        'memtable_usage_bytes', 'memtable_budget_bytes'):
                overlaps = [resource + '.cache_usage_bytes'] if key == 'cache_pinned_bytes' else []
                kind = 'capacity' if 'budget' in key or 'capacity' in key else 'counter'
                metric(resource + '.' + key, raw[key], resource, kind, overlaps)
            for key in ('cache_usage_bytes', 'memtable_usage_bytes'):
                contributions[resource + '.' + key] = raw[key]
        for key in ('active_memtables_bytes', 'all_memtables_bytes', 'retained_memtables_bytes'):
            metric(owner + '.' + key, raw.get(key), owner, overlaps=[resource + '.memtable_usage_bytes'],
                   reason='overlapping RocksDB memtable estimates; not added to subtotal')
        table = raw.get('table_readers_estimate_bytes')
        metric(owner + '.table_readers_estimate_bytes', table, owner,
               reason='RocksDB estimate excludes block cache; nonresident capacity possible')
        contributions[owner + '.table_readers_estimate_bytes'] = table
        for key in ('page_snapshot_bytes', 'query_buffer_bytes', 'transaction_buffer_bytes', 'metadata_cache_bytes'):
            metric(owner + '.' + key, None, owner, reason=raw['accounting'].get(key, 'not instrumented'))
        for key in ('page_sessions', 'page_live_snapshots', 'rocksdb_snapshots', 'active_query_executions', 'metadata_cache_collections'):
            metric(owner + '.' + key, raw.get(key), owner, 'counter', unit='count')
    seen_managers, pmr = set(), None
    for manager in managers:
        if id(manager) in seen_managers:
            continue
        seen_managers.add(id(manager))
        raw = json.loads(manager.memory_report())
        owner = f'graph_manager:{len(graphs)}'
        graphs.append({'owner': owner, 'raw': raw})
        pmr = raw['snapshot_pmr_process']  # Process-wide; never sum manager copies.
        for index, graph in enumerate(raw['graphs']):
            graph_owner = owner + f'.graph:{index}'
            if graph['loaded']:
                for key, value in graph['live'].items():
                    kind = 'capacity' if key == 'budget_bytes' else 'reservation' if key == 'admission_charged_bytes' else 'estimate'
                    metric(graph_owner + '.' + key, value, graph_owner, kind,
                           [] if key == 'total_bytes_estimate' else [graph_owner + '.total_bytes_estimate'])
                contributions[graph_owner + '.total_bytes_estimate'] = graph['live']['total_bytes_estimate']
            metric(graph_owner + '.cached_snapshot_owned_requested_bytes', graph['cached_snapshot_owned_requested_bytes'],
                   graph_owner, 'counter', ['graph_snapshot_pmr.current_requested_bytes'])
        metric(owner + '.scheduler_reserved_bytes', raw['scheduler']['reserved_bytes'], owner, 'reservation',
               ['graph_snapshot_pmr.current_requested_bytes'], 'budget reservation, not allocation')
        for key in ('queued', 'running'):
            metric(owner + '.scheduler_' + key, raw['scheduler'].get(key), owner, 'counter', unit='count')
        metric(owner + '.query_scratch_bytes', None, owner, reason='algorithm workspace/results not fully instrumented')
    if native is not None and hasattr(native, 'graph_snapshot_memory'):
        pmr = dict(native.graph_snapshot_memory())
    for key in ('current_requested_bytes', 'peak_requested_bytes', 'live_resources'):
        metric('graph_snapshot_pmr.' + key, pmr[key] if pmr else None, 'process',
               'high_water' if key.startswith('peak') else 'counter',
               reason='all native snapshot PMR resources; excludes admission baselines and allocator overhead',
               unit='count' if key == 'live_resources' else 'bytes')
    if pmr:
        contributions['graph_snapshot_pmr.current_requested_bytes'] = pmr['current_requested_bytes']
    for key, value in allocator.items():
        if key.endswith('_bytes'):
            metric('allocator.' + key, value, 'process', 'estimate',
                   ['native and Python allocations'], allocator['coverage'] if 'coverage' in allocator else allocator['error'])
    for key in ('traced_current_bytes', 'traced_peak_bytes', 'tracer_overhead_bytes', 'total_live_bytes'):
        metric('python.' + key, python[key], 'process', 'high_water' if 'peak' in key else 'counter',
               ['allocator.allocated_arena_bytes', 'allocator.mmap_bytes'], python['coverage'])
    complete = all(value is not None for value in contributions.values())
    subtotal = sum(contributions.values()) if contributions and complete else None
    rss, pss = os_memory['rss_bytes'], os_memory['pss_bytes']
    return {'schema_version': 1, 'pid': os.getpid(), 'started_monotonic_ns': started,
            'sample_seconds': (time.monotonic_ns() - started) / 1e9,
            'process': os_memory, 'allocator': allocator, 'python': python,
            'documents': documents, 'graph_managers': graphs, 'measurements': measurements,
            'reconciliation': {'process_total_internal_bytes': None,
                'selected_native_estimate_bytes': subtotal, 'contributions': contributions,
                'rss_minus_selected_native_estimates_bytes': rss - subtotal if rss is not None and subtotal is not None else None,
                'pss_minus_selected_native_estimates_bytes': pss - subtotal if pss is not None and subtotal is not None else None,
                'meaning': 'signed comparison, NOT unattributed resident bytes: estimates/requests may be nonresident; process includes Python, allocator slack, stacks, code, mmap and unknown scratch',
                'excluded_from_subtotal': ['capacities', 'reservations', 'pinned cache subset', 'per-DB memtable aliases',
                    'cached snapshots already in process PMR', 'Python traces and allocator totals (overlap native)'],
                'missing_attribution': ['query/results/transaction buffers', 'metadata and graph manager/ID-store maps',
                    'allocator rounding/tcache/free pages', 'Python untraced allocations', 'stacks/code/libraries/direct mmap'],
                'sampling': 'sequential counters, not atomic; RSS/PSS from same rollup; no GC, flush, cache clear or malloc_trim; native report can expire idle cursors'}}
