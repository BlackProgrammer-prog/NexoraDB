"""Low-frequency process diagnostics; allocator numbers are not live object sizes."""
import ctypes
import sys
import tracemalloc
import threading
import time

_trim_lock = threading.Lock()
_last_trim = float('-inf')


def reclaim_allocator(force=False):
    """Return unused glibc pages, never objects/cache or transaction state.

    Explicit native deployment maintenance; timed costs remain in resource data.
    Unsupported allocators are reported. Free arena bytes are not all reclaimable.
    """
    global _last_trim
    if not _trim_lock.acquire(blocking=False):
        return {'attempted': False, 'reason': 'maintenance_busy'}
    try:
        now = time.monotonic()
        if not force and now-_last_trim < 5:
            return {'attempted': False, 'reason': 'rate_limited'}
        before = python_memory()['glibc']
        if before is None:
            return {'attempted': False, 'reason': 'allocator_unsupported'}
        if (before['free_arena_bytes'] < 256*1024**2 or
                before['free_arena_bytes'] < before['arena_bytes']//4):
            return {'attempted': False, 'reason': 'below_retention_threshold'}
        try:
            trim = ctypes.CDLL(None).malloc_trim
        except (AttributeError, OSError):
            return {'attempted': False, 'reason': 'trim_unsupported'}
        trim.argtypes = [ctypes.c_size_t]
        trim.restype = ctypes.c_int
        _last_trim = now
        started = time.perf_counter()
        result = trim(0)
        return {'attempted': True, 'returned': result,
                'seconds': time.perf_counter()-started,
                'before': before, 'after': python_memory()['glibc']}
    finally:
        _trim_lock.release()


def python_memory():
    result = {'allocated_blocks': sys.getallocatedblocks(),
              'tracemalloc_enabled': tracemalloc.is_tracing(),
              'scope': 'glibc process-wide allocator, includes native and Python malloc; '
                       'free arenas may contain fragmentation and cannot all be reclaimed'}
    if tracemalloc.is_tracing():
        result['traced_current_bytes'], result['traced_peak_bytes'] = tracemalloc.get_traced_memory()
    class Mallinfo(ctypes.Structure):
        _fields_ = [(name, ctypes.c_size_t) for name in (
            'arena', 'ordblks', 'smblks', 'hblks', 'hblkhd', 'usmblks',
            'fsmblks', 'uordblks', 'fordblks', 'keepcost')]
    try:
        library = ctypes.CDLL(None)
        query = library.mallinfo2
        query.restype = Mallinfo
        query.argtypes = []
        value = query()
        result['glibc'] = {'arena_bytes': value.arena, 'allocated_arena_bytes': value.uordblks,
                           'free_arena_bytes': value.fordblks, 'mmap_bytes': value.hblkhd,
                           'top_releasable_estimate_bytes': value.keepcost}
    except (AttributeError, OSError):
        result['glibc'] = None
    return result
