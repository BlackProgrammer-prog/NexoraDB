"""Bounded ingestion-buffer A/B. Never runs or edits a 100M experiment."""
import argparse
import json
from pathlib import Path
import statistics
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'nexora-scale-lab'))


def metrics(engine):
    native = engine.rocks_metrics()
    result = {name: getattr(native, name) for name in
            ('logical_write_bytes', 'wal_write_bytes', 'flush_write_bytes',
             'compaction_write_bytes', 'pending_compaction_bytes', 'stall_micros',
             'l0_files', 'memtable_usage', 'memtable_budget', 'running_compactions')}
    operations=json.loads(engine.operation_metrics())
    result['write_path']=operations['write_path']
    result['batch_insert']=operations['operations']['batch_insert']
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rows', type=int, default=300000)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--buffers-mib', nargs='+', type=int, default=[16, 64])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--input-api', choices=['list', 'packed', 'ingest'], default='list')
    parser.add_argument('--batch-size', type=int, choices=[1000,5000,10000], default=1000)
    parser.add_argument('--processes', type=int, choices=[0,1,2,4], default=0)
    parser.add_argument('--wal-range-sync-mib', type=int, choices=[0,1,4], default=1)
    parser.add_argument('--allow-5m-probe', action='store_true')
    parser.add_argument('--namespace', default='items')
    args = parser.parse_args()
    maximum=5000000 if args.allow_5m_probe else 1000000
    if not 1000 <= args.rows <= maximum or not 1 <= args.repetitions <= 5:
        parser.error('rows 1000..1M (5M only with --allow-5m-probe); repetitions 1..5')
    if len(args.buffers_mib) > 4 or any(not 1 <= n <= 128 for n in args.buffers_mib):
        parser.error('at most four buffer choices, each 1..128MiB')
    if args.output.exists():
        parser.error('refusing to overwrite previous evidence')
    import nexoradb
    from bson import BSON
    from scale_lab.dataset import DatasetConfig
    from scale_lab.loading import load_batches
    from scale_lab.adapters import stored
    import fcntl
    with (ROOT / 'nexora-scale-lab/state/protocol.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runs = []
        for repeat in range(args.repetitions):
            for size in (args.buffers_mib if repeat % 2 == 0 else list(reversed(args.buffers_mib))):
                with tempfile.TemporaryDirectory(prefix='nexora-buffer-probe-') as folder:
                    settings = nexoradb.RocksSettings()
                    settings.memory_budget_bytes = 5120 * 1024**2
                    settings.block_cache_bytes = 4096 * 1024**2
                    settings.write_buffer_bytes = size * 1024**2
                    settings.durability = nexoradb.DocumentDurability.Durable
                    settings.wal_bytes_per_sync = args.wal_range_sync_mib * 1024**2
                    engine = nexoradb.DocEngine(folder, rocks_settings=settings)
                    assert engine.create_collection(args.namespace).success
                    index = nexoradb.IndexDefinition()
                    index.index_name = 'username_idx'
                    index.fields = ['username']
                    index.type = nexoradb.IndexType.SingleField
                    assert engine.create_index(args.namespace, index).success
                    samples = []
                    started, ack_seconds, encode_seconds = time.perf_counter(), 0., 0.
                    dataset=DatasetConfig(args.rows,batch_size=args.batch_size)
                    if args.processes:
                        from scale_lab.encoded_loading import encoded_batches
                        pipeline=encoded_batches(dataset,args.processes)
                    else:
                        pipeline=load_batches(dataset,2)
                    with pipeline as prepared:
                        for batch, digest in prepared:
                            tick = time.perf_counter()
                            encoded = batch if args.processes else [BSON.encode(stored(value)) for value in batch]
                            encode_seconds += time.perf_counter() - tick
                            tick = time.perf_counter()
                            result = (engine.insert_many_bson_ingest(args.namespace,encoded) if args.input_api=='ingest' else
                                      engine.insert_many_bson_buffer(args.namespace,b''.join(encoded))
                                      if args.input_api=='packed' else engine.insert_many_bson(args.namespace,encoded))
                            ack_seconds += time.perf_counter() - tick
                            assert result.success, result.error_msg
                            acknowledged=int(result.data) if args.input_api=='ingest' else len(json.loads(result.data))
                            assert acknowledged==len(encoded)
                            if digest.count % 100000 == 0 or digest.count == args.rows:
                                samples.append(dict(records=digest.count, seconds=time.perf_counter()-started,
                                                    **metrics(engine)))
                    elapsed = time.perf_counter()-started
                    before_flush = metrics(engine)
                    tick = time.perf_counter()
                    assert engine.flush_storage().success
                    flush_seconds = time.perf_counter()-tick
                    row = dict(repetition=repeat, write_buffer_mib=size, rows=args.rows,
                               seconds=elapsed, records_per_second=args.rows/elapsed,
                               native_ack_seconds=ack_seconds, bson_encode_seconds=encode_seconds,
                               flush_seconds=flush_seconds, before_flush=before_flush,
                               after_flush=metrics(engine), samples=samples)
                    # Verification outside timed loading; no record is skipped
                    # just to meet a throughput target.
                    assert engine.get_collection_size(args.namespace)==args.rows
                    from scale_lab.dataset import record
                    indices=sorted({i*(args.rows-1)//99 for i in range(100)})
                    for index in indices:
                        actual=engine.find_by_id(args.namespace,f'{index:012d}')
                        assert actual.success
                        value=json.loads(actual.data);value.pop('_id')
                        assert value==record(dataset,index)
                    row['validation']={'exact_count':args.rows,'sampled_records':len(indices),
                                       'full_integrity':False,'baseline_digest':digest.result()}
                    print(json.dumps(row), flush=True)
                    assert engine.prepare_shutdown().success
                    del engine
                    runs.append(row)
        report = dict(input_api=args.input_api,batch_size=args.batch_size,producer_processes=args.processes,
                      namespace=args.namespace,
                      wal_range_sync_mib=args.wal_range_sync_mib,
                      scope='bounded local durable BSON ingestion with username index; generation/digest/prefetch included; flush reported separately; not a 100M extrapolation',
                      runs=runs, medians={str(size): statistics.median(
                          row['records_per_second'] for row in runs if row['write_buffer_mib']==size)
                          for size in args.buffers_mib})
        with args.output.open('x', encoding='utf-8') as output:
            json.dump(report, output, indent=2)
        print(json.dumps({'medians': report['medians']}))


if __name__ == '__main__':
    main()
