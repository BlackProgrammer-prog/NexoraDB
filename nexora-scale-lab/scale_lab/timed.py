"""Bounded closed/open-loop trials and deterministic disjoint-key write replay."""
import hashlib
import math
import random
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

from .dataset import record
from .runner import Latencies


def access_rng(seed, worker):
    return random.Random(int.from_bytes(hashlib.sha256(f'{seed}:{worker}'.encode()).digest(), 'big'))


def zipf_rank(rng, size, theta):
    # Exact discrete Zipf rejection sampler: continuous x^-theta proposal on
    # [1,N+1), floor to k, accept (x/k)^theta / 2^theta. The accepted density
    # integrates to a constant times k^-theta on each unit-width interval.
    scale = (size + 1) ** (1 - theta) - 1
    while True:
        x = (1 + rng.random() * scale) ** (1 / (1 - theta))
        k = min(size, max(1, int(x)))
        if rng.random() <= (x / k) ** theta / 2 ** theta:
            return k - 1


def operation(config, dataset, workload, clients, working_set, seed, worker, sequence, rng):
    size = int(dataset.records * working_set)
    slots = (size - worker + clients - 1) // clients
    if config.distribution == 'zipfian':
        rank = zipf_rank(rng, slots, config.zipf_theta)
        # Reversible rank scrambling prevents Zipf popularity from simply being
        # ascending physical keys, without allocating a 100M-element permutation.
        multiplier = (abs(seed) * 2 + 1) % slots if slots > 1 else 1
        while math.gcd(multiplier, slots) != 1:
            multiplier = (multiplier + 1) % slots
        rank = (multiplier * rank + seed) % slots
    else:
        rank = rng.randrange(slots)
    index = worker + clients * rank
    probability = {'ycsb-a': .5, 'ycsb-b': .05, 'ycsb-c': 0, 'indexed-read': 0}[workload]
    update = rng.random() < probability
    status = f'bench:{seed}:{worker}:{sequence}' if update else None
    return index, status


def trial(adapter, dataset, config, workload, clients, working_set, seed, duration,
          max_operations=None, monitor=None, progress_callback=None, status_callback=None):
    shared = {}
    progress_states = [None] * clients
    if monitor is not None and progress_callback is not None:
        def checkpoint_progress():
            counters = Counter()
            buckets = Counter()
            for state in progress_states:
                if state is not None:
                    counters.update(dict(state['counts']))
                    buckets.update(dict(state['histogram'].buckets))
            progress_callback({'status': 'in-progress-not-certified', 'counts': dict(counters),
                               'response_histogram_buckets': dict(buckets),
                               'note': 'Non-atomic periodic snapshot; interrupted suffix outcomes are unknown.'})
        monitor.progress_callback = checkpoint_progress
    release_cleanup = threading.Event()
    stop_status = threading.Event()
    status_thread = None

    def pulse():
        while not stop_status.wait(1):
            counts = Counter()
            for state in progress_states:
                if state is not None:
                    counts.update(dict(state['counts']))
            try:
                status_callback({'elapsed_seconds': max(0, time.perf_counter() - shared['start']),
                                 'counts': dict(counts)})
            except Exception:
                # UI diagnostics cannot silently invalidate benchmark evidence.
                shared['status_error'] = True
                return
    completion = threading.Barrier(clients + 1, timeout=duration + 90)

    def begin():
        nonlocal status_thread
        if monitor is not None:
            monitor.__enter__()
            shared['monitor_started'] = True
        shared['start'] = time.perf_counter() + .01
        if status_callback is not None:
            status_thread = threading.Thread(target=pulse, daemon=True)
            status_thread.start()

    barrier = threading.Barrier(clients + 1, timeout=60, action=begin)

    def worker(worker_id):
        rng = access_rng(seed, worker_id)
        counts = Counter()
        response, service = Latencies(), Latencies()
        progress_states[worker_id] = {'counts': counts, 'histogram': response}
        errors = []
        validation_ns = 0
        sequence = 0
        try:
            with adapter.worker_session():
                barrier.wait()
                start = shared['start']
                deadline = start + duration
                if start > time.perf_counter():
                    time.sleep(max(0, start - time.perf_counter()))
                while True:
                    ordinal = worker_id + sequence * clients
                    if max_operations is not None and ordinal >= max_operations:
                        break
                    scheduled = (start + ordinal / config.target_rate
                                 if config.load_mode == 'open-loop' else time.perf_counter())
                    if config.load_mode == 'open-loop':
                        if scheduled >= deadline:
                            break
                    elif scheduled >= deadline:
                        break
                    delay = scheduled - time.perf_counter()
                    if delay > 0:
                        time.sleep(delay)
                    index, status = operation(config, dataset, workload, clients, working_set,
                                              seed, worker_id, sequence, rng)
                    now = time.perf_counter()
                    if config.load_mode == 'open-loop' and now - scheduled > config.max_queue_delay_seconds:
                        counts.update(attempted=1, failed=1, timeouts=1, rejected_load_slots=1)
                        response.add(int((now - scheduled) * 1e9))
                        sequence += 1
                        continue
                    invoked = time.perf_counter_ns()
                    try:
                        if status is not None:
                            adapter.update(f'{index:012d}', status)
                            value = None
                        elif workload == 'indexed-read':
                            values = adapter.read_by_field('username', f'user_{index}')
                            value = values[0] if len(values) == 1 else None
                        else:
                            value = adapter.read_by_id(f'{index:012d}')
                        returned = time.perf_counter_ns()
                        service.add(returned - invoked)
                        if status is None:
                            expected = record(dataset, index)
                            valid_status = value.get('status') if isinstance(value, dict) else None
                            # A/B updates are disjoint across workers. A read can
                            # see baseline or an earlier status authored by itself.
                            if valid_status != expected['status']:
                                prefix = f'bench:{seed}:{worker_id}:'
                                if (not isinstance(valid_status, str) or not valid_status.startswith(prefix)
                                        or not valid_status[len(prefix):].isdigit()
                                        or int(valid_status[len(prefix):]) >= sequence
                                        or workload not in {'ycsb-a', 'ycsb-b'}):
                                    raise ValueError('Unexpected status in point read')
                                expected['status'] = valid_status
                            if value != expected:
                                raise ValueError('Unexpected logical record')
                        validation_ns += time.perf_counter_ns() - returned
                        counts['successful'] += 1
                        counts['updates' if status is not None else 'reads'] += 1
                    except Exception as error:
                        if 'returned' not in locals() or returned < invoked:
                            service.add(time.perf_counter_ns() - invoked)
                        counts['failed'] += 1
                        if isinstance(error, ValueError):
                            counts['unexpected_results'] += 1
                        if isinstance(error, TimeoutError) or 'timeout' in type(error).__name__.lower() or getattr(error, 'sqlstate', None) == '57014':
                            counts['timeouts'] += 1
                        if len(errors) < 5:
                            # Error text from drivers can include credentials;
                            # persist only class/code in publication artifacts.
                            errors.append({'type': type(error).__name__, 'code': getattr(error, 'code', None),
                                           'sqlstate': getattr(error, 'sqlstate', None)})
                    finally:
                        counts['attempted'] += 1
                        latency_start = scheduled if config.load_mode == 'open-loop' else invoked / 1e9
                        response.add(int((time.perf_counter() - latency_start) * 1e9))
                    sequence += 1
                completion.wait()
                release_cleanup.wait()
        except BaseException:
            barrier.abort()
            completion.abort()
            release_cleanup.set()
            raise
        return {'worker': worker_id, 'counts': dict(counts), 'response': response,
                'service': service, 'error_examples': errors, 'validation_ns': validation_ns}

    with ThreadPoolExecutor(max_workers=clients) as executor:
        futures = [executor.submit(worker, i) for i in range(clients)]
        try:
            barrier.wait()
            completion.wait()
            if max_operations is None:
                remaining = shared['start'] + duration - time.perf_counter()
                if remaining > 0:
                    time.sleep(remaining)
            ended = time.perf_counter()
        finally:
            stop_status.set()
            if status_thread is not None:
                status_thread.join()
            if shared.get('monitor_started'):
                monitor.__exit__(None, None, None)
            release_cleanup.set()
        results = [future.result() for future in futures]
    elapsed = max(1e-9, ended - shared['start'])
    counts = Counter()
    histograms = {'response_latency': Latencies(), 'driver_latency': Latencies()}
    for result in results:
        counts.update(result['counts'])
        for label, key in [('response_latency', 'response'), ('driver_latency', 'service')]:
            aggregate, partial = histograms[label], result[key]
            aggregate.buckets.update(partial.buckets)
            aggregate.count += partial.count
            aggregate.total += partial.total
            aggregate.maximum = max(aggregate.maximum, partial.maximum)
    if status_callback is not None:
        status_callback({'elapsed_seconds': elapsed, 'counts': dict(counts)})
    if shared.get('status_error'):
        raise RuntimeError('Progress status persistence failed; inspect filesystem health')
    return {'workload': workload, 'clients': clients, 'working_set': working_set, 'access_seed': seed,
            'load_mode': config.load_mode, 'distribution': config.distribution,
            'target_rate': config.target_rate if config.load_mode == 'open-loop' else None,
            'key_ownership': 'disjoint worker partitions; no cross-client key contention',
            'requested_seconds': duration, 'elapsed_seconds_including_drain': elapsed,
            'max_operations': max_operations, 'counts': dict(counts),
            'throughput': {key + '_per_second': counts[key] / elapsed
                           for key in ('attempted', 'successful', 'failed')},
            'latency': histograms['response_latency'].summary(),
            'histograms': {key: {'summary': value.summary(), 'buckets': dict(value.buckets),
                                 'count': value.count, 'total_ns': value.total}
                           for key, value in histograms.items()},
            'workers': [{'worker': r['worker'], 'counts': r['counts']} for r in results],
            'error_examples': [e for r in results for e in r['error_examples']],
            'client_validation_ns': sum(r['validation_ns'] for r in results)}


def replay_writes(writes, dataset, config, result, progress=None):
    if result['counts'].get('failed', 0):
        raise RuntimeError('Cannot certify final write state after failed/unknown outcomes')
    completed = 0
    for worker in result['workers']:
        rng = access_rng(result['access_seed'], worker['worker'])
        for sequence in range(worker['counts'].get('attempted', 0)):
            index, status = operation(config, dataset, result['workload'], result['clients'],
                                      result['working_set'], result['access_seed'], worker['worker'], sequence, rng)
            if status is not None:
                writes.put(f'{index:012d}', status)
            completed += 1
            if progress is not None and completed % dataset.batch_size == 0:
                progress(completed)
    writes.connection.commit()
    if progress is not None:
        progress(completed)
