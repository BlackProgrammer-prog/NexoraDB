"""Initial closed-loop runner. Results are smoke results, not publication trials."""
import math
import random
import time
import json
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

from .dataset import batches, record


class Latencies:
    """Logarithmic histogram, upper-bound quantiles with at most 1% bucket error."""
    def __init__(self):
        self.buckets = Counter()
        self.count = self.total = self.maximum = 0

    def add(self, nanoseconds):
        nanoseconds = max(1, nanoseconds)
        self.buckets[math.ceil(math.log(nanoseconds, 1.01))] += 1
        self.count += 1
        self.total += nanoseconds
        self.maximum = max(self.maximum, nanoseconds)

    def summary(self):
        result = {'average_ms': self.total / max(1, self.count) / 1e6,
                  'maximum_ms': self.maximum / 1e6, 'quantile_relative_bucket_error': 0.01}
        for label, fraction in [('p50', .5), ('p90', .9), ('p95', .95),
                                ('p99', .99), ('p999', .999)]:
            target = math.ceil(self.count * fraction)
            accumulated = 0
            result[label + '_ms'] = None
            for bucket, count in sorted(self.buckets.items()):
                accumulated += count
                if accumulated >= target:
                    result[label + '_ms'] = min(self.maximum, 1.01 ** bucket) / 1e6
                    break
        return result


def validate(adapter, config, samples=100):
    count = adapter.count()
    if count != config.records:
        raise RuntimeError(f'Record count mismatch: {count} != {config.records}')
    indices = sorted({0, config.records - 1} | {
        random.Random(config.seed + i).randrange(config.records) for i in range(samples)})
    for index in indices:
        expected = record(config, index)
        if adapter.read_by_id(expected['id']) != expected:
            raise RuntimeError(f'Data integrity failure at {index}')
    return {'record_count': count, 'validated_records': len(indices),
            'validation': 'sampled logical equality; not a full database checksum'}


def load(adapter, config):
    # Refuse append/retry: a failed batch can have an unknown committed prefix.
    if adapter.count() != 0:
        raise RuntimeError('Loading requires an empty namespace; resumable loading is not implemented')
    count = 0
    started = time.perf_counter()
    last_progress = started
    for batch in batches(config):
        adapter.batch_insert(batch)
        count += len(batch)
        now = time.perf_counter()
        if now - last_progress >= 5 or count == config.records:
            print(json.dumps({'event': 'load_progress', 'database': adapter.engine_name,
                              'inserted': count, 'target': config.records,
                              'elapsed_seconds': now - started}), file=sys.stderr, flush=True)
            last_progress = now
    elapsed = time.perf_counter() - started
    verification = validate(adapter, config)
    return {'records': count, 'elapsed_seconds': elapsed,
            'records_per_second': count / elapsed, 'validation': verification,
            'includes_generation_and_serialization': True}


def run(adapter, config, workload='ycsb-c', clients=1, operations=1000):
    if workload not in {'ycsb-a', 'ycsb-b', 'ycsb-c'}:
        raise ValueError('NOT_SUPPORTED: workload')
    if clients < 1 or operations < 1:
        raise ValueError('clients and operations must be positive')
    baseline = validate(adapter, config)
    write_probability = {'ycsb-a': .5, 'ycsb-b': .05, 'ycsb-c': 0}[workload]

    def worker(worker_id):
        rng = random.Random(config.seed + worker_id)
        histogram = Latencies()
        counters = Counter()
        errors = []
        for _ in range(worker_id, operations, clients):
            key = f'{rng.randrange(config.records):012d}'
            update = rng.random() < write_probability
            started = time.perf_counter_ns()
            try:
                if update:
                    adapter.update(key, 'benchmark_active')
                    counters['updates'] += 1
                else:
                    value = adapter.read_by_id(key)
                    if value is None or value.get('id') != key:
                        counters['unexpected_results'] += 1
                        raise RuntimeError('Point lookup returned an unexpected record')
                    counters['reads'] += 1
                counters['successful'] += 1
            except Exception as error:
                counters['failed'] += 1
                if isinstance(error, TimeoutError) or 'timeout' in type(error).__name__.lower():
                    counters['timeouts'] += 1
                if len(errors) < 5:
                    errors.append({'type': type(error).__name__, 'message': str(error)[:300]})
            finally:
                histogram.add(time.perf_counter_ns() - started)
                counters['attempted'] += 1
        return histogram, counters, errors

    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=clients) as executor:
        results = list(executor.map(worker, range(clients)))
    elapsed = time.perf_counter() - started
    histogram = Latencies()
    counters = Counter()
    errors = []
    for partial, counts, examples in results:
        histogram.buckets.update(partial.buckets)
        histogram.count += partial.count
        histogram.total += partial.total
        histogram.maximum = max(histogram.maximum, partial.maximum)
        counters.update(counts)
        errors.extend(examples)
    final_count = adapter.count()
    return {'database': adapter.engine_name, 'workload': workload,
            'workload_family': 'YCSB-inspired', 'distribution': 'uniform',
            'clients': clients, 'elapsed_seconds': elapsed, 'counts': dict(counters),
            'throughput': {key + '_per_second': counters[key] / elapsed
                           for key in ('attempted', 'successful', 'failed')},
            'latency': histogram.summary(), 'error_examples': errors[:10],
            'baseline_validation': baseline, 'final_record_count': final_count,
            'record_count_mismatch': final_count != config.records,
            'dataset_modified': write_probability > 0,
            'measurement_mode': 'closed-loop', 'publication_eligible': False,
            'limitations': ['No warm-up, repetitions or state restoration in this smoke runner.',
                           'All attempted latencies include failures; no coordinated-omission correction.',
                           'Native embedded transport differs from PostgreSQL/MongoDB network transport.',
                           'MongoDB ordered batches can partially commit; other adapters use atomic batches.',
                           'Server resource monitoring and full integrity verification are pending.']}
