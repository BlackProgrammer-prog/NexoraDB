"""Bounded local controls/advice; no result cache, model calls or automatic DDL."""
from collections import OrderedDict
import hashlib
import json
import re
from threading import RLock
from weakref import WeakKeyDictionary

from .parser import configure_parser_cache, parser_cache_stats
from . import ast_nodes as N

_LOCK = RLock()
_STATES = WeakKeyDictionary()
_CONTROLS = {'parser_cache', 'clear_parser_cache', 'read_cache_admission', 'smart_indexes', 'index_advisor'}


def _state(engine):
    return _STATES.setdefault(engine, {'enabled': True, 'samples': OrderedDict()})


def configure_performance(engine, options):
    if not isinstance(options, dict) or set(options) - _CONTROLS:
        raise ValueError('Unknown performance control')
    if any(type(value) is not bool for value in options.values()):
        raise ValueError('Performance controls require booleans')
    with _LOCK:
        if {'read_cache_admission', 'smart_indexes'} & options.keys():
            if not hasattr(engine, 'configure_query_tuning'):
                raise ValueError('Rebuild the native engine for query tuning controls')
            current = json.loads(engine.query_tuning())
            engine.configure_query_tuning(options.get('read_cache_admission', current['read_cache_admission']),
                                          options.get('smart_indexes', current['smart_indexes']))
        if 'parser_cache' in options or options.get('clear_parser_cache'):
            configure_parser_cache(options.get('parser_cache', parser_cache_stats()['enabled']),
                                   options.get('clear_parser_cache', False))
        state = _state(engine)
        if 'index_advisor' in options:
            state['enabled'] = options['index_advisor']
            if not state['enabled']:
                state['samples'].clear()
        return performance_status(engine)


def observe_select(engine, statement, seconds):
    with _LOCK:
        try:
            state = _state(engine)
        except TypeError:
            return  # Lightweight/fake adapters need not support weak references.
        if not state or not state['enabled'] or statement.joins or statement.collection.startswith('__nexora_'):
            return
        pending, fields = [statement.where], set()
        while pending:
            node = pending.pop()
            if isinstance(node, N.Cmp) and node.op in {'EQ', 'GT', 'GTE', 'LT', 'LTE'}:
                if node.field != '_id':
                    fields.add(node.field)
            elif isinstance(node, N.And):
                pending.extend(node.subs)
        samples = state['samples']
        for field in sorted(fields):
            key = (statement.collection, field)
            if key not in samples:
                if len(samples) >= 128:
                    samples.popitem(last=False)
                samples[key] = {'observations': 0, 'total_seconds': 0.0}
            samples[key]['observations'] += 1
            samples[key]['total_seconds'] += seconds
            samples.move_to_end(key)


def index_advice(engine):
    with _LOCK:
        state = _state(engine)
        if not state['enabled']:
            return []
        result = []
        for (collection, field), sample in state['samples'].items():
            if sample['observations'] < 3:
                continue
            if not all(re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', value) for value in (collection, field)):
                continue
            indexes = engine.get_indexes(collection)
            if any(list(index.fields)[:1] == [field] for index in indexes):
                continue
            name = 'advice_' + hashlib.sha256((collection+'\0'+field).encode()).hexdigest()[:12]
            result.append(dict(collection=collection, field=field, **sample,
                               reason='Repeated equality/range predicate without a leading-field index; heuristic, not a proven speedup.',
                               query=f'CREATE INDEX {name} ON {collection} ({field});',
                               requires_confirmation=True))
        return sorted(result, key=lambda item: item['total_seconds'], reverse=True)[:20]


def performance_status(engine):
    with _LOCK:
        native = json.loads(engine.query_tuning()) if hasattr(engine, 'query_tuning') else {}
        metrics = engine.rocks_metrics() if native and hasattr(engine, 'rocks_metrics') else None
        storage_cache = ({key: getattr(metrics, key) for key in
                          ('cache_capacity', 'cache_usage', 'cache_pinned', 'cache_hits', 'cache_misses',
                           'memtable_budget', 'memtable_usage', 'pending_compaction_bytes', 'stall_micros')}
                         if metrics is not None else None)
        return dict(parser_cache=parser_cache_stats(),
                    document_durability=(json.loads(engine.durability_report())
                                         if hasattr(engine,'durability_report') else None),
                    storage_cache=storage_cache,
                    write_path=(json.loads(engine.operation_metrics()).get('write_path')
                                if native and hasattr(engine,'operation_metrics') else None),
                    read_cache_admission=native.get('read_cache_admission'),
                    smart_indexes=native.get('smart_indexes'),
                    native_tuning_available=bool(native), index_advisor=_state(engine)['enabled'],
                    advice=index_advice(engine), advisor_mode='local deterministic heuristic, not an AI model',
                    runtime_only=True, index_constraints_always_enforced=True,
                    cache_semantics='Admission off prevents new data blocks being cached; existing hits/metadata caching remain possible. No result cache.')
