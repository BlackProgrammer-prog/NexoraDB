"""Bounded generation/checksum prefetch; only consumers acknowledge progress."""
from contextlib import contextmanager
from queue import Queue, Full
from threading import Event, Thread
import time

from .dataset import batches
from .integrity import ContentDigest


@contextmanager
def load_batches(dataset, prefetch=0, timings=None):
    if type(prefetch) is not int or not 0 <= prefetch <= 8:
        raise ValueError('prefetch must be an integer in [0, 8]')

    def prepared():
        digest = ContentDigest()
        iterator = iter(batches(dataset))
        for _ in range((dataset.records + dataset.batch_size - 1) // dataset.batch_size):
            tick = time.perf_counter()
            batch = next(iterator)
            if timings is not None:
                timings['generation'] += time.perf_counter() - tick
            tick = time.perf_counter()
            for value in batch:
                digest.add(value)
            if timings is not None:
                timings['digest'] += time.perf_counter() - tick
            yield batch, ContentDigest(digest.count, digest.total, digest.xor)

    if not prefetch:
        yield prepared()
        return
    queue = Queue(maxsize=prefetch)
    stop = Event()

    def put(value):
        while not stop.is_set():
            try:
                queue.put(value, timeout=.05)
                return True
            except Full:
                pass
        return False

    def produce():
        try:
            for item in prepared():
                if stop.is_set() or not put(('batch', item)):
                    return
            put(('end', None))
        except BaseException as error:
            put(('error', error))

    def consume():
        while True:
            kind, value = queue.get()
            if kind == 'error':
                raise value
            if kind == 'end':
                return
            yield value

    thread = Thread(target=produce, name='scale-lab-load-producer', daemon=True)
    thread.start()
    try:
        yield consume()
    finally:
        stop.set()
        thread.join()  # Producer's bounded puts observe cancellation.
