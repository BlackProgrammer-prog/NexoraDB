"""Small reproducible external-sort quota/cleanup and bounded-growth campaign. No user stores."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import tracemalloc

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'NexoraDB/src/nexoradb/execution.py'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); sys.modules[name] = module; spec.loader.exec_module(module)
    return module


def worker(args):
    native = load('nexoradb', args.native)
    module = load('bounded_probe', SOURCE)
    pool = native.ResourceBudget(32*1024**2)
    class Engine:
        def execution_budget(self): return pool
    checksum_in = checksum_out = 0
    modulus = 2**256
    def digest(row):
        return int.from_bytes(hashlib.sha256(json.dumps(row, sort_keys=True).encode()).digest(), 'big')
    def source():
        nonlocal checksum_in
        for i in range(args.rows):
            row = {'_id':f'{i:09}', 'key':(args.rows-i)%113, 'payload':'فارسی\n'+'x'*32}
            checksum_in = (checksum_in + digest(row)) % modulus
            yield row
    with tempfile.TemporaryDirectory(prefix='nexora-owned-spill-probe-') as private:
        pipe = module.QueryPipeline(Engine(), memory_bytes=1024**2, max_record_bytes=256,
                                   disk_budget=module.SpillDiskBudget(64*1024**2), spill_parent=private)
        tracemalloc.start(1)
        count, previous = 0, None
        with pipe.sort(source(), 'key') as rows:
            for row in rows:
                key = (row['key'], row['_id'])
                if previous is not None and key < previous: raise AssertionError('sort oracle mismatch')
                previous = key; count += 1
                checksum_out = (checksum_out + digest(row)) % modulus
        current, peak = tracemalloc.get_traced_memory(); tracemalloc.stop()
        assert count == args.rows and checksum_in == checksum_out
        assert not list(Path(private).iterdir())
        assert json.loads(pool.report())['reserved_bytes'] == 0 and pipe.disk.report()['reserved_bytes'] == 0
        result = {'rows':count, 'multiset_sha256_sum':f'{checksum_out:064x}', 'ordered':True,
            'operator_memory_quota_bytes':pipe.memory, 'python_traced_current_bytes':current,
            'python_traced_peak_bytes':peak, 'rss_high_water_bytes':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*1024,
            'admission':json.loads(pool.report()), 'spill':pipe.disk.report(), 'runs':pipe.last_spill, 'cleanup':True}
    print(json.dumps(result))


def campaign(args):
    args.directory.mkdir(parents=True, exist_ok=False)
    report = {'schema':1, 'status':'running', 'native_sha256':hashlib.sha256(args.native.read_bytes()).hexdigest(),
              'execution_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(), 'cases':[],
              'scope':'fresh processes; bounded Python operator buffers and disk credits, not hard RSS'}
    try:
        for count in (10000, 100000):
            result = subprocess.run([sys.executable,str(Path(__file__).resolve()),'--worker', '--native',str(args.native),
                '--directory',str(args.directory),'--rows',str(count)], capture_output=True,text=True,timeout=180)
            (args.directory/f'{count}.stderr').write_text(result.stderr)
            if result.returncode: raise RuntimeError(result.stderr)
            case = json.loads(result.stdout); report['cases'].append(case)
            print(f'{count} rows: quota, sort oracle and cleanup passed',flush=True)
        small, large = report['cases']
        assert large['python_traced_peak_bytes'] <= small['python_traced_peak_bytes'] + 2*1024**2
        assert large['python_traced_peak_bytes'] < 8*1024**2
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed',error=str(error)); raise
    finally:
        (args.directory/'report.json').write_text(json.dumps(report,indent=2))


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory',type=Path,required=True)
    parser.add_argument('--native',type=Path,default=ROOT/'build/debug/nexoradb.cpython-310-x86_64-linux-gnu.so')
    parser.add_argument('--worker',action='store_true'); parser.add_argument('--rows',type=int,default=10000)
    args=parser.parse_args(); args.native=args.native.resolve(strict=True)
    if not 1<=args.rows<=100000: parser.error('rows must be 1..100000')
    worker(args) if args.worker else campaign(args)
