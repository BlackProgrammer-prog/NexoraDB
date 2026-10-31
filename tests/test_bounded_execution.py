import errno
import importlib.util
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import threading
import pytest
from test_resource_budget import native, ROOT, report

SPEC = importlib.util.spec_from_file_location('bounded_execution_test', ROOT/'NexoraDB/src/nexoradb/execution.py')
execution = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(execution)


class Engine:
    def __init__(self, pool, rows=()): self.pool, self.rows, self.open_streams = pool, rows, 0
    def execution_budget(self): return self.pool
    def iter_documents(self, *_, **__):
        owner = self; owner.open_streams += 1
        class Stream:
            def __init__(self): self.rows = iter(owner.rows); self.closed = False
            def __iter__(self): return self
            def __next__(self): return next(self.rows)
            def close(self):
                if not self.closed: owner.open_streams -= 1; self.closed = True
        return Stream()


def data(count=3000):
    for i in range(count): yield {'_id': f'{i:06}', 'v': (count-i) % 31, 'n': i, 'text': 'فارسی\n'+('x'*32)}


def pipeline(native, parent, rows=(), **kwargs):
    engine = Engine(native.ResourceBudget(16*1024**2), rows)
    return execution.QueryPipeline(engine, memory_bytes=1024**2, max_record_bytes=256, fan_in=4,
        disk_budget=execution.SpillDiskBudget(16*1024**2), spill_parent=parent, **kwargs)


def clean(pipe, parent):
    assert list(parent.iterdir()) == []
    assert pipe.disk.report()['reserved_bytes'] == 0
    assert report(pipe.pool)['reserved_bytes'] == 0


def test_sizing_includes_all_explicit_margins():
    plan = execution.size_execution_budget(process_bytes=1024, rocks_bytes=256, allocator_bytes=128,
        python_bytes=128, page_cache_bytes=128, safety_bytes=64)
    assert plan['execution_bytes'] == 320 and 'not hard RSS' in plan['scope']
    with pytest.raises(execution.ResourceLimitError):
        execution.size_execution_budget(process_bytes=100, rocks_bytes=100, allocator_bytes=1, python_bytes=1, page_cache_bytes=1, safety_bytes=1)


def test_multilevel_external_sort_group_count_sum_and_cleanup(native, tmp_path):
    pipe = pipeline(native, tmp_path)
    expected = sorted(data(), key=lambda row: (row['v'], row['_id']))
    with pipe.sort(data(), 'v') as rows:
        assert list(rows) == expected
        assert all(path.stat().st_mode & 0o777 == 0o700 for path in tmp_path.iterdir())
    assert pipe.last_spill['runs_created'] > 16
    clean(pipe, tmp_path)
    with pipe.aggregate(data(), 'v', sum_field='n') as groups:
        actual = list(groups)
    assert len(actual) == 31
    for group in actual:
        source = [row for row in expected if row['v'] == group['key']]
        assert group['count'] == len(source) and group['sum'] == sum(row['n'] for row in source)
    assert pipe.disk.report()['peak_reserved_bytes'] <= pipe.disk.capacity
    clean(pipe, tmp_path)


def test_scalar_order_missing_null_bool_numeric_aliases_and_precision(native, tmp_path):
    pipe = pipeline(native, tmp_path)
    source = [{'_id':'missing'}, {'_id':'null','v':None}, {'_id':'bool','v':True},
        {'_id':'b','v':1.0}, {'_id':'a','v':1}, {'_id':'hi','v':2**53+1}, {'_id':'lo','v':float(2**53)}, {'_id':'str','v':'1'}]
    with pipe.sort(reversed(source), 'v') as rows:
        assert [row['_id'] for row in rows] == ['missing','null','bool','a','b','lo','hi','str']
    with pipe.aggregate(source, 'v') as rows: groups = list(rows)
    assert groups[0]['key_present'] is False and groups[1]['key_present'] is True
    assert groups[3]['count'] == 2
    clean(pipe, tmp_path)


def test_scan_close_materialized_result_lifetime_and_quota_failure(native, tmp_path):
    pipe = pipeline(native, tmp_path, rows=list(data()))
    with pipe.scan('items') as rows: next(rows)
    assert pipe.engine.open_streams == 0
    result = pipe.materialize('items', limit=3)
    assert len(result) == 3 and 0 < report(pipe.pool)['components']['query'] <= pipe.memory
    result.close()
    assert report(pipe.pool)['reserved_bytes'] == 0
    with pytest.raises(execution.ResourceLimitError): pipe.materialize('items')
    assert pipe.engine.open_streams == 0
    clean(pipe, tmp_path)


def test_cancellation_and_early_exit_release_all_resources(native, tmp_path):
    cancelled = threading.Event()
    pipe = pipeline(native, tmp_path, cancelled=cancelled.is_set)
    with pytest.raises(execution.QueryCancelled):
        with pipe.sort(data(), 'v') as rows:
            next(rows); cancelled.set(); next(rows)
    clean(pipe, tmp_path)
    cancelled.clear()
    with pipe.sort(data(), 'v') as rows: next(rows)
    clean(pipe, tmp_path)


def test_spill_quota_includes_merge_coexistence_and_rounded_blocks(native, tmp_path):
    pipe = pipeline(native, tmp_path); pipe.disk = execution.SpillDiskBudget(8192)
    with pytest.raises(execution.ResourceLimitError):
        with pipe.sort(data(), 'v') as rows: list(rows)
    clean(pipe, tmp_path)


@pytest.mark.parametrize('failure', [errno.ENOSPC, errno.EACCES, errno.EIO])
def test_partial_spill_io_errors_never_succeed_and_cleanup(native, tmp_path, monkeypatch, failure):
    pipe = pipeline(native, tmp_path)
    original = execution._Workspace.new
    class Broken:
        def __init__(self, stream): self.stream = stream
        def __enter__(self): return self
        def __exit__(self, *_): self.stream.close()
        def write(self, frame):
            self.stream.write(frame[:5]); raise OSError(failure, 'injected partial spill failure')
    def faulty(self):
        path, stream = original(self); return path, Broken(stream)
    monkeypatch.setattr(execution._Workspace, 'new', faulty)
    with pytest.raises(OSError) as caught:
        with pipe.sort(data(), 'v') as rows: list(rows)
    assert caught.value.errno == failure
    clean(pipe, tmp_path)


def test_corrupt_spill_is_not_silently_skipped(native, tmp_path):
    pipe = pipeline(native, tmp_path)
    with pytest.raises(execution.SpillCorruption):
        with pipe.sort(data(), 'v') as rows:
            next(rows)
            for path in tmp_path.glob('*/run-*'):
                with path.open('r+b') as target:
                    target.seek(-2, os.SEEK_END); byte = target.read(1)
                    target.seek(-2, os.SEEK_END); target.write(bytes([byte[0] ^ 1]))
            list(rows)
    clean(pipe, tmp_path)


def test_spill_creation_and_enter_failure_cleanup(native, tmp_path, monkeypatch):
    pipe = pipeline(native, tmp_path)
    real_open = execution.os.open
    def denied(path, flags, *args, **kwargs):
        if Path(path).name.startswith('run-'): raise PermissionError(errno.EACCES, 'injected creation error')
        return real_open(path, flags, *args, **kwargs)
    monkeypatch.setattr(execution.os, 'open', denied)
    with pytest.raises(PermissionError):
        with pipe.sort(data(), 'v') as rows: list(rows)
    clean(pipe, tmp_path)
    monkeypatch.setattr(execution.os, 'statvfs', lambda _: (_ for _ in ()).throw(OSError(errno.EIO, 'statvfs fault')))
    with pytest.raises(OSError):
        with pipe.sort([], 'v'): pass
    clean(pipe, tmp_path)


@pytest.mark.parametrize('source, error', [([{'_id':'a','v':[]}], ValueError),
    ([{'_id':'a','v':1,'text':'x'*10000}], execution.ResourceLimitError),
    ([{'_id':'a','v':1,'n':2**63-1},{'_id':'b','v':1,'n':1}], OverflowError)])
def test_type_record_and_sum_overflow_fail_without_temp_leak(native, tmp_path, source, error):
    pipe = pipeline(native, tmp_path)
    with pytest.raises(error):
        with pipe.aggregate(source, 'v', sum_field='n') as rows: list(rows)
    clean(pipe, tmp_path)


def test_sigkill_orphan_recovery_preserves_active_unknown_and_symlink(native, tmp_path):
    worker = '''import importlib.util,sys,signal
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path); m=importlib.util.module_from_spec(s); sys.modules[name]=m; s.loader.exec_module(m); return m
n=load('nexoradb',sys.argv[1]); e=load('spill_worker',sys.argv[2])
class Engine:
 def execution_budget(self): return n.ResourceBudget(16*1024**2)
p=e.QueryPipeline(Engine(),memory_bytes=1024**2,max_record_bytes=256,spill_parent=sys.argv[3])
with p.sort(({'_id':str(i),'v':3000-i,'payload':'x'*32} for i in range(3000)), 'v') as rows:
 next(rows); print('READY',flush=True); signal.pause()
'''
    child = subprocess.Popen([sys.executable, '-c', worker, str(ROOT/'build/debug/nexoradb.cpython-310-x86_64-linux-gnu.so'),
        str(ROOT/'NexoraDB/src/nexoradb/execution.py'), str(tmp_path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ)
            assert selector.select(10), 'worker did not reach spill boundary'
            assert child.stdout.readline().strip() == 'READY'
        child.kill(); assert child.wait(timeout=5) == -signal.SIGKILL
    finally:
        if child.poll() is None: child.kill(); child.wait(timeout=5)
        child.stdout.close(); child.stderr.close()
    pipe = pipeline(native, tmp_path)
    unrelated = tmp_path/'keep-user-data'; unrelated.mkdir()
    link = tmp_path/'nexora-spill-999999-0-link'; link.symlink_to(unrelated, target_is_directory=True)
    with pipe.sort([], 'v'):
        removed = execution.cleanup_abandoned_spills(tmp_path)
        assert len(removed) == 1
        assert len(list(tmp_path.glob('nexora-spill-*'))) == 2  # active owner + preserved symlink
    assert unrelated.is_dir() and link.is_symlink()
