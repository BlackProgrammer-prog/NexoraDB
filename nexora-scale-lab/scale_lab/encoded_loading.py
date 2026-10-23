"""Bounded spawn-only BSON/checksum producers, never fork an open native DB."""
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from contextlib import contextmanager
import multiprocessing

from .dataset import record
from .integrity import ContentDigest, MODULUS


def _prepare(dataset, start, stop):
    from bson import BSON
    encoded, digest = [], ContentDigest()
    for index in range(start, stop):
        value = record(dataset, index)
        digest.add(value)
        encoded.append(BSON.encode({**value, '_id': value['id']}))
    return encoded, digest


@contextmanager
def encoded_batches(dataset, processes=2):
    if type(processes) is not int or not 1 <= processes <= 4:
        raise ValueError('BSON producer processes must be 1..4')
    # Forking a process after RocksDB opens is unsafe. Spawn gives workers no
    # inherited engine, locks, sockets or transaction handles.
    executor = ProcessPoolExecutor(max_workers=processes, mp_context=multiprocessing.get_context('spawn'))
    pending, offsets = deque(), iter(range(0, dataset.records, dataset.batch_size))
    def submit():
        first = next(offsets, None)
        if first is None:
            return False
        pending.append(executor.submit(_prepare, dataset, first, min(first+dataset.batch_size,dataset.records)))
        return True
    def consume():
        cumulative = ContentDigest()
        for _ in range(processes*2):
            if not submit(): break
        while pending:
            encoded, partial = pending.popleft().result()
            cumulative.count += partial.count
            cumulative.total = (cumulative.total+partial.total) % MODULUS
            cumulative.xor ^= partial.xor
            # Logical order and checksums exactly match the serial generator.
            # The caller may expose this count only after durable ACK.
            yield encoded, ContentDigest(cumulative.count,cumulative.total,cumulative.xor)
            submit()
    try:
        yield consume()
    finally:
        for future in pending: future.cancel()
        executor.shutdown(wait=True,cancel_futures=True)
