#!/usr/bin/env python3
"""Bounded durable write diagnostic, not a publishable benchmark or automatic tuner."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'NexoraDB/src'))


def cell(directory, clients, batch, independent):
    from bson import BSON
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import load_native_module
    native=load_native_module(AdminApiSettings())
    rocks=native.RocksSettings(); rocks.memory_budget_bytes=32*1024**2
    rocks.durability=native.DocumentDurability.Durable
    engine=native.DocEngine(str(directory/'db'),native.TransactionSettings(),rocks)
    engine.pause_document_migration()
    records=320
    namespaces=[f'items_{i}' if independent else 'items' for i in range(clients)]
    for name in set(namespaces):
        assert engine.create_collection(name).success
        index=native.IndexDefinition();index.index_name='key_idx';index.fields=['key'];index.type=native.IndexType.SingleField
        assert engine.create_index(name,index).success
    rows=[[{'_id':f'{worker}-{i:04}', 'key':f'{worker}-{i:04}', 'payload':'x'*128}
           for i in range(records//clients)] for worker in range(clients)]
    buffers=[[b''.join(BSON.encode(row) for row in source[i:i+batch]) for i in range(0,len(source),batch)] for source in rows]
    ready=threading.Barrier(clients+1); start=threading.Barrier(clients+1)
    def write(worker):
        latencies=[];ready.wait();start.wait()
        for buffer in buffers[worker]:
            begin=time.perf_counter_ns()
            result=engine.insert_many_bson_buffer(namespaces[worker],buffer)
            latencies.append(time.perf_counter_ns()-begin)
            assert result.success,result.error_msg
        return latencies
    try:
        with ThreadPoolExecutor(max_workers=clients) as pool:
            jobs=[pool.submit(write,i) for i in range(clients)]
            ready.wait()
            before=json.loads(engine.operation_metrics())['write_path']
            begin=time.perf_counter_ns();start.wait()
            latencies=[value for job in jobs for value in job.result()]
            elapsed=(time.perf_counter_ns()-begin)/1e9
        after=json.loads(engine.operation_metrics())['write_path']
        actual=[]
        for name in set(namespaces):
            with engine.iter_documents(name) as stream: current=list(stream)
            assert engine.get_collection_size(name)==len(current)
            actual.extend(current)
        assert sorted(actual,key=lambda row:row['_id'])==sorted([row for group in rows for row in group],key=lambda row:row['_id'])
        stages={name:{key:after['stages'][name][key]-before['stages'][name][key] for key in ('calls','total_ns')}
                for name in after['stages']}
        calls=len(latencies);syncs=after['wal_syncs']-before['wal_syncs']
        latencies.sort()
        report={'clients':clients,'batch_records':batch,'collections':'independent' if independent else 'shared',
                'records':records,'commit_calls':calls,'elapsed_seconds':elapsed,'records_s':records/elapsed,
                'p50_ack_us':statistics.median(latencies)/1000,
                'p99_ack_us':latencies[max(0,(99*len(latencies)+99)//100-1)]/1000,
                'wal_syncs':syncs,'commits_per_wal_sync':calls/syncs if syncs else None,
                'wal_sync_total_us':after['wal_sync_latency']['sum_us']-before['wal_sync_latency']['sum_us'],
                'stages':stages,'correct':True,'effective_durability':json.loads(engine.durability_report()),
                'scope':'native embedded threads, 320 records; no warm-up; diagnostic only; stages overlap',
                'publication_eligible':False}
        assert syncs>0 and report['effective_durability']['sync_on_ack']
        assert engine.prepare_shutdown().success
        return report
    finally:
        del engine


def main():
    if not __debug__: raise RuntimeError('Do not run with python -O')
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory',type=Path,required=True)
    parser.add_argument('--repetitions',type=int,choices=(1,2,3),default=2)
    parser.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--clients',type=int,choices=(1,4,10),default=1)
    parser.add_argument('--batch',type=int,choices=(1,8),default=1)
    parser.add_argument('--independent',action='store_true')
    args=parser.parse_args()
    if args.worker:
        print(json.dumps(cell(args.directory,args.clients,args.batch,args.independent))); return
    directory=args.directory.resolve()
    if directory.is_relative_to(ROOT) and not directory.is_relative_to(ROOT/'build'):
        raise ValueError('Use fresh build/ output or an external directory')
    directory.mkdir(parents=True,exist_ok=False)
    reports=[]
    for repetition in range(args.repetitions):
        for independent in (False,True):
            for clients in (1,4,10):
                for batch in (1,8):
                    name=f'{repetition}-{int(independent)}-{clients}-{batch}'
                    path=directory/name;path.mkdir()
                    command=[sys.executable,str(Path(__file__).resolve()),'--worker','--directory',str(path),
                             '--clients',str(clients),'--batch',str(batch)]
                    if independent: command.append('--independent')
                    print('starting '+name,flush=True)
                    with (path/'stdout.json').open('x') as stdout,(path/'stderr.log').open('x') as stderr:
                        subprocess.run(command,stdout=stdout,stderr=stderr,check=True,timeout=30)
                    report=json.loads((path/'stdout.json').read_text());report['repetition']=repetition
                    reports.append(report)
                    print(json.dumps({key:report[key] for key in ('clients','batch_records','collections','records_s','p99_ack_us','commits_per_wal_sync')}),flush=True)
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import load_native_module
    native=load_native_module(AdminApiSettings())
    binary=Path(getattr(native,'__native_module_path__',native.__file__))
    with (directory/'report.json').open('x') as output:
        json.dump({'schema':1,'native_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                   'publication_eligible':False,'samples':reports},output,indent=2)


if __name__=='__main__':main()
