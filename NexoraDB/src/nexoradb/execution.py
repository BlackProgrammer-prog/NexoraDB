"""Bounded read operators and private quota-controlled external merge spill.

Explicit Python API contract; this module does not add SQL ORDER BY/GROUP BY syntax.
All iterators must be used in their context manager. Consumer-retained objects are
outside operator ownership. Admission credits are not a process RSS limit.
"""
from contextlib import contextmanager, ExitStack, closing
import heapq
import json
import math
import os
from pathlib import Path
import shutil
import stat
import tempfile
import threading
import zlib


class QueryCancelled(InterruptedError):
    code = 'query_cancelled'


class ResourceLimitError(MemoryError):
    code = 'resource_exhausted'


class SpillCorruption(ValueError):
    code = 'spill_corruption'


def size_execution_budget(*, process_bytes, rocks_bytes, allocator_bytes, python_bytes,
                          page_cache_bytes, safety_bytes):
    """Explicit deployment envelope. Page cache is charged cgroup memory, not RSS.

    No guessed machine limit: callers supply their container/host envelope and
    observed margins. Cache/memtable budgets must be counted once per resource.
    """
    values = locals().copy()
    if any(type(value) is not int or value < 0 for value in values.values()):
        raise ValueError('sizing requires nonnegative integer bytes')
    available = process_bytes - sum(value for name, value in values.items() if name != 'process_bytes')
    if available <= 0:
        raise ResourceLimitError('no execution headroom after Rocks/Python/allocator/page-cache/safety margins')
    return {**values, 'execution_bytes': available,
            'scope': 'admission sizing, not hard RSS enforcement; cgroup limits remain independent'}


class SpillDiskBudget:
    """Share this object between pipelines to cap concurrent spill occupancy."""
    def __init__(self, capacity_bytes):
        if type(capacity_bytes) is not int or capacity_bytes <= 0:
            raise ValueError('spill capacity must be positive')
        self.capacity = capacity_bytes
        self.used = self.peak = 0
        self.rejected = 0
        self.lock = threading.Lock()

    def claim(self, size):
        with self.lock:
            if size > self.capacity - self.used:
                self.rejected += 1
                raise ResourceLimitError('spill disk quota exceeded (including merge input/output coexistence)')
            self.used += size
            self.peak = max(self.peak, self.used)

    def release(self, size):
        with self.lock:
            self.used -= size

    def report(self):
        with self.lock:
            return dict(capacity_bytes=self.capacity, reserved_bytes=self.used, peak_reserved_bytes=self.peak,
                        rejected_total=self.rejected,
                        scope='rounded data blocks plus per-file metadata allowance; not a filesystem hard quota')


def _start_ticks(pid):
    return (Path('/proc') / str(pid) / 'stat').read_text().rsplit(')', 1)[1].split()[19]


def cleanup_abandoned_spills(parent):
    """Explicit Linux recovery; only our private UID-owned PID/start-tick directories.

    Active or inaccessible processes and symlinks are preserved. Configure a
    dedicated application-owned spill parent, never a shared user data directory.
    PID/start ticks are in the atomically-created directory name, before any files.
    """
    removed = []
    for path in Path(parent).glob('nexora-spill-*'):
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
            continue
        pieces = path.name.split('-')
        if len(pieces) != 5 or not pieces[2].isdigit() or not pieces[3].isdigit():
            continue
        try:
            if _start_ticks(int(pieces[2])) == pieces[3]:
                continue
        except FileNotFoundError:
            pass
        except (PermissionError, ProcessLookupError):
            continue
        shutil.rmtree(path)
        removed.append(path.name)
    return removed


class _Workspace:
    def __init__(self, parent, budget):
        self.parent, self.budget = parent, budget
        self.files = {}
        self.lengths = {}
        self.sequence = self.runs = self.written = 0

    def __enter__(self):
        prefix = f'nexora-spill-{os.getpid()}-{_start_ticks(os.getpid())}-'
        self.directory = Path(tempfile.mkdtemp(prefix=prefix, dir=self.parent))
        try: self.block = max(4096, os.statvfs(self.directory).f_frsize)
        except BaseException:
            shutil.rmtree(self.directory); raise
        return self

    def __exit__(self, *_):
        shutil.rmtree(self.directory)  # Failure is explicit; remaining files keep their disk charge.
        for size in self.files.values():
            self.budget.release(size)
        self.files.clear()
        self.lengths.clear()

    def new(self):
        self.sequence += 1; self.runs += 1
        path = self.directory / f'run-{self.sequence:016d}'
        self.budget.claim(self.block)  # Per-file metadata allowance, plus rounded data blocks below.
        try: fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        except BaseException:
            self.budget.release(self.block); raise
        self.files[path] = self.block; self.lengths[path] = 0
        return path, os.fdopen(fd, 'wb', buffering=0)

    def write(self, path, stream, payload):
        frame = f'{zlib.crc32(payload):08x} '.encode() + payload + b'\n'
        length = self.lengths[path] + len(frame)
        charge = self.block + ((length + self.block - 1) // self.block) * self.block
        self.budget.claim(charge - self.files[path])
        self.files[path] = charge; self.lengths[path] = length
        if stream.write(frame) != len(frame):
            raise OSError('short spill write')
        self.written += len(frame)

    def delete(self, path):
        path.unlink()
        self.budget.release(self.files.pop(path))
        del self.lengths[path]


_MISSING = object()


class BudgetedRows(list):
    """Returned response owns credits until clear/close/destruction; copies by consumers are external ownership."""
    def __init__(self, credit):
        super().__init__(); self._credit = credit

    def close(self):
        self.clear(); self._credit = None

    def __del__(self):
        self.close()

    def projected(self, rows):
        result = BudgetedRows(self._credit)
        result.extend(rows)
        return result


def _field(row, path):
    value = row
    for name in path.split('.'):
        if not isinstance(value, dict) or name not in value:
            return _MISSING
        value = value[name]
    return value


def _key(value):
    if value is _MISSING: return (0, 0)
    if value is None: return (1, 0)
    if type(value) is bool: return (2, value)
    if type(value) is int:
        if not -(2**63) <= value < 2**63: raise ValueError('sort/group integer outside int64')
        return (3, value)
    if type(value) is float and math.isfinite(value): return (3, value)
    if type(value) is str: return (4, value)
    raise ValueError('sort/group supports missing, null, bool, int64, finite double and string scalars only')


def _preflight(value, remaining, depth=0):
    remaining -= 2
    if remaining < 0 or depth > 64:
        raise ResourceLimitError('record exceeds bounded preflight quota/depth')
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str): raise ValueError('JSON object keys must be strings')
            remaining = _preflight(key, remaining, depth + 1)
            remaining = _preflight(child, remaining, depth + 1)
    elif isinstance(value, list):
        for child in value: remaining = _preflight(child, remaining, depth + 1)
    elif isinstance(value, str):
        remaining -= len(value)
        if remaining < 0: raise ResourceLimitError('record exceeds bounded string quota')
    elif type(value) is int:
        if not -(2**63) <= value < 2**63: raise ValueError('JSON integer outside int64')
    elif type(value) is float:
        if not math.isfinite(value): raise ValueError('nonfinite JSON number')
    elif value is not None and type(value) is not bool:
        raise ValueError('unsupported JSON value')
    return remaining


class QueryPipeline:
    """Scan, ascending stable external sort, grouped COUNT/checked int64 SUM.

    Sort key order: missing < null < bool < number < Unicode string. Numbers use
    exact Python int/double comparison (bool is separate). Tie: _id, input sequence.
    Group missing/null remain distinct. The native predicate still decides scan
    membership. One page snapshot is retained for the complete native scan.
    """
    def __init__(self, engine, *, memory_bytes=16*1024**2, disk_budget=None, spill_parent=None,
                 max_record_bytes=8192, fan_in=4, cancelled=None):
        if type(memory_bytes) is not int or memory_bytes < 1024**2:
            raise ValueError('operator memory quota must be at least 1MiB')
        if not 2 <= fan_in <= 16 or not 32 <= max_record_bytes <= memory_bytes // (128 * (fan_in + 1)):
            raise ValueError('record/fan-in quota leaves insufficient bounded merge workspace')
        self.engine = engine
        self.pool = engine.execution_budget()
        self.memory = memory_bytes
        self.disk = disk_budget if disk_budget is not None else SpillDiskBudget(256*1024**2)
        self.parent = str(Path(spill_parent).resolve(strict=True)) if spill_parent else None
        self.max_record, self.fan_in = max_record_bytes, fan_in
        self.cancelled = cancelled if cancelled is not None else lambda: False
        self.last_spill = {}

    def _check(self):
        if self.cancelled():
            raise QueryCancelled('query cancelled')

    def materialize(self, collection, condition=None, *, limit=0, skip=0):
        credit = self.pool.reserve('query', self.memory)
        result = BudgetedRows(credit)
        try:
            with self.scan(collection, condition) as rows:
                used = 0
                for index, row in enumerate(rows):
                    if index < skip: continue
                    size = len(json.dumps(row, ensure_ascii=False, allow_nan=False).encode())
                    cost = size * 32 + 1024
                    if cost > self.memory - used:
                        raise ResourceLimitError('result quota exceeded; use bounded streaming scan')
                    result.append(row); used += cost
                    if limit and len(result) >= limit: break
            credit.resize(used)
            return result
        except BaseException:
            result.close(); credit.close(); raise

    def materialize_records(self, records, *, skip=0):
        credit = self.pool.reserve('query', self.memory)
        result = BudgetedRows(credit)
        try:
            used = 0
            for index, raw in enumerate(records):
                self._check()
                if index < skip: continue
                cost = len(raw) * 4 * 32 + 1024  # UTF-8 worst case before making a bytes copy.
                if cost > self.memory - used: raise ResourceLimitError('result decode quota exceeded')
                result.append(json.loads(raw)); used += cost
            credit.resize(used)
            return result
        except BaseException:
            result.close(); credit.close(); raise

    @contextmanager
    def scan(self, collection, condition=None):
        with self.pool.reserve('buffer', min(self.memory // 4, 512*1024)):
            options = dict(page_size=128, max_page_bytes=min(self.memory // 64, 64*1024))
            if condition is not None: options['condition'] = condition
            stream = self.engine.iter_documents(collection, **options)
            try:
                def rows():
                    for row in stream:
                        self._check()
                        yield row
                yield rows()
            finally:
                stream.close()

    def _item(self, sequence, row):
        if not isinstance(row, dict) or not isinstance(row.get('_id'), str):
            raise ValueError('sort rows require a string _id')
        # Reject oversized/deep input before materializing another unbounded JSON copy.
        _preflight(row, self.max_record)
        payload = json.dumps([sequence, row], ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode()
        if len(payload) > self.max_record:
            raise ResourceLimitError('record exceeds bounded operator record quota')
        return (_key(_field(row, self._sort_field)), row['_id'], sequence, payload)

    def _read(self, stream):
        self._check()
        frame = stream.readline(self.max_record + 11)
        if not frame: return None
        if len(frame) > self.max_record + 10 or not frame.endswith(b'\n') or frame[8:9] != b' ':
            raise SpillCorruption('invalid/truncated spill frame')
        payload = frame[9:-1]
        try:
            if int(frame[:8], 16) != zlib.crc32(payload): raise SpillCorruption('spill checksum mismatch')
            seq, row = json.loads(payload)
            if type(seq) is not int or seq < 0: raise SpillCorruption('invalid spill sequence')
            item = self._item(seq, row)
            if item[3] != payload: raise SpillCorruption('noncanonical spill payload')
            return item
        except (ValueError, TypeError, KeyError) as error:
            raise SpillCorruption(str(error)) from error

    def _merge(self, paths):
        with ExitStack() as stack:
            streams = [stack.enter_context(path.open('rb', buffering=0)) for path in paths]
            heap = []
            for index, stream in enumerate(streams):
                item = self._read(stream)
                if item is not None: heapq.heappush(heap, (item, index))
            while heap:
                self._check()
                item, index = heapq.heappop(heap)
                yield item
                item = self._read(streams[index])
                if item is not None: heapq.heappush(heap, (item, index))

    def _merged_run(self, workspace, paths):
        path, output = workspace.new()
        with output, closing(self._merge(paths)) as merged:
            for item in merged: workspace.write(path, output, item[3])
        for old in paths: workspace.delete(old)
        return path

    def _sorted(self, source, workspace):
        levels, buffer, used = [[] for _ in range(32)], [], 0
        input_count = 0
        self._active_buffer = buffer
        def add(path):
            for level in levels:
                level.append(path)
                if len(level) < self.fan_in: return
                path = self._merged_run(workspace, level); level.clear()
            raise ResourceLimitError('external sort level limit exceeded')
        def flush():
            buffer.sort()
            path, output = workspace.new()
            with output:
                for item in buffer:
                    self._check(); workspace.write(path, output, item[3])
            buffer.clear(); add(path)
        for sequence, row in enumerate(source):
            self._check()
            if sequence >= 2**63: raise ResourceLimitError('sort sequence overflow')
            item = self._item(sequence, row)
            cost = len(item[3]) * 32 + 1024
            if buffer and used + cost > self.memory // 2: flush(); used = 0
            buffer.append(item); used += cost
            input_count += 1
        if not any(levels):
            buffer.sort()
            for item in buffer:
                self._check(); yield json.loads(item[3])[1]
            return
        if buffer: flush()
        paths = [path for level in levels for path in level]
        while len(paths) > self.fan_in:
            paths = [self._merged_run(workspace, paths[start:start+self.fan_in])
                     for start in range(0, len(paths), self.fan_in)]
        with closing(self._merge(paths)) as merged:
            emitted = 0
            for item in merged:
                emitted += 1; yield json.loads(item[3])[1]
            if emitted != input_count: raise SpillCorruption('spill row count mismatch')

    @contextmanager
    def sort(self, rows, field):
        # A pipeline owns one operation at a time; use separate instances sharing pool/disk for concurrency.
        if hasattr(self, '_sort_field'): raise RuntimeError('pipeline already executing')
        if not isinstance(field, str) or not field: raise ValueError('sort field must be a nonempty dotted path')
        self._sort_field = field
        try:
            with self.pool.reserve('query', self.memory), _Workspace(self.parent, self.disk) as workspace:
                source = iter(rows); ordered = self._sorted(source, workspace)
                try: yield ordered
                finally:
                    try: ordered.close()
                    finally:
                        try:
                            if hasattr(source, 'close'): source.close()
                        finally:
                            if hasattr(self, '_active_buffer'):
                                self._active_buffer.clear(); del self._active_buffer
                            self.last_spill = dict(runs_created=workspace.runs, bytes_written=workspace.written)
        finally:
            del self._sort_field

    @contextmanager
    def aggregate(self, rows, group_by, *, sum_field=None):
        with self.sort(rows, group_by) as ordered:
            def groups():
                previous = None; current = None
                for row in ordered:
                    value = _field(row, group_by); key = _key(value)
                    if current is None or key != previous:
                        if current is not None: yield current
                        current = {'key': None if value is _MISSING else value,
                                   'key_present': value is not _MISSING, 'count': 0}
                        if sum_field is not None: current['sum'] = 0
                        previous = key
                    current['count'] += 1
                    if sum_field is not None:
                        number = _field(row, sum_field)
                        if type(number) is not int: raise ValueError('SUM requires present int64 values, excluding bool')
                        total = current['sum'] + number
                        if not -(2**63) <= number < 2**63 or not -(2**63) <= total < 2**63:
                            raise OverflowError('SUM int64 overflow')
                        current['sum'] = total
                if current is not None: yield current
            result = groups()
            try: yield result
            finally: result.close()
