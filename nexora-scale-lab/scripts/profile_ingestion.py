"""Bounded ingestion diagnostics, never the 100M study. Sources results stay separate."""
import argparse
import fcntl
import json
import os
import statistics
import tempfile
import time
import uuid
from pathlib import Path

from scale_lab.adapters import NexoraAdapter, PostgreSQLAdapter, MongoDBAdapter, native_result, stored
from scale_lab.dataset import DatasetConfig, batches
from scale_lab.integrity import ContentDigest
from scale_lab.loading import load_batches
from scale_lab.protocol import LAB, binary_digest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--records', type=int, default=100000)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--engines', nargs='+', choices=['nexoradb', 'mongodb', 'postgresql'],
                        default=['nexoradb', 'mongodb', 'postgresql'])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--without-index', action='store_true')
    parser.add_argument('--native-bson', action='store_true')
    parser.add_argument('--prefetch', type=int, choices=range(9), default=0)
    args = parser.parse_args()
    if not 0 < args.records <= 1000000 or not 0 < args.repetitions <= 5:
        parser.error('Diagnostic bounded to 1M records and 5 repetitions per engine')
    if args.output.exists():
        parser.error('Output already exists; preserve earlier observations')
    with (LAB / 'state/protocol.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        rows = []
        # Alternate engine order to expose first-run effects, not a publication claim.
        for repetition in range(args.repetitions):
            engines = args.engines if repetition % 2 == 0 else list(reversed(args.engines))
            for name in engines:
                namespace = 'scale_lab_profile_' + uuid.uuid4().hex
                with tempfile.TemporaryDirectory(prefix='scale-lab-profile-') as folder:
                    if name == 'nexoradb':
                        adapter = NexoraAdapter(str(Path(folder) / 'db'), namespace)
                    elif name == 'mongodb':
                        adapter = MongoDBAdapter(os.environ['SCALE_LAB_MONGO_URI'], namespace)
                    elif name == 'postgresql':
                        adapter = PostgreSQLAdapter(os.environ['SCALE_LAB_POSTGRES_DSN'], namespace)
                    else:
                        raise ValueError(name)
                    adapter.connect()
                    try:
                        adapter.prepare_schema()
                        if not args.without_index:
                            adapter.create_indexes()
                        digest = ContentDigest()
                        phases = dict(generation=0., serialization=0., insert_ack=0., digest=0.)
                        config = DatasetConfig(args.records)
                        started = time.perf_counter()
                        with load_batches(config, args.prefetch, phases) as prepared:
                            for batch, prepared_digest in prepared:
                                if name == 'nexoradb':
                                    tick = time.perf_counter()
                                    if args.native_bson:
                                        from bson import BSON
                                        encoded = [BSON.encode(stored(v)) for v in batch]
                                    else:
                                        encoded = [json.dumps(stored(v), allow_nan=False) for v in batch]
                                    phases['serialization'] += time.perf_counter() - tick
                                    tick = time.perf_counter()
                                    insert = adapter.engine.insert_many_bson if args.native_bson else adapter.engine.insert_many
                                    ids = json.loads(native_result(insert(namespace, encoded)))
                                    del insert  # Do not retain a prior engine across repetitions/shutdown.
                                    assert len(ids) == len(batch)
                                else:
                                    tick = time.perf_counter()
                                    adapter.batch_insert(batch)
                                phases['insert_ack'] += time.perf_counter() - tick
                                digest = prepared_digest
                        elapsed = time.perf_counter() - started
                        assert adapter.count() == args.records
                        from scale_lab.dataset import record
                        for i in (0, args.records // 2, args.records - 1):
                            assert adapter.read_by_id(f'{i:012d}') == record(config, i)
                        row = dict(engine=name, repetition=repetition, records=args.records,
                                   seconds=elapsed, records_per_second=args.records/elapsed,
                                   phases_seconds=phases, digest=digest.result())
                        if name == 'nexoradb':
                            metrics = adapter.engine.rocks_metrics()
                            row['rocks'] = {key: getattr(metrics, key) for key in
                                ('stall_micros', 'wal_write_bytes', 'compaction_write_bytes',
                                 'cache_hits', 'cache_misses', 'cache_usage', 'cache_capacity')}
                            row['binary_sha256'] = binary_digest(Path(adapter.native.__file__))
                        rows.append(row)
                        print(json.dumps(row), flush=True)
                    finally:
                        adapter.cleanup()
                        adapter.disconnect()
        result = dict(scope='bounded diagnostic, not a 100M/publication result',
                      native_bson=args.native_bson,
                      prefetch_batches=args.prefetch,
                      secondary_index=not args.without_index, rows=rows,
                      medians={name: statistics.median(r['records_per_second'] for r in rows if r['engine']==name)
                               for name in args.engines})
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x') as output:
            json.dump(result, output, indent=2)
        print(json.dumps(result['medians']))


if __name__ == '__main__':
    main()
