"""Small durable update diagnostic. Never runs the 100M study."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import tempfile
import time
import nexoradb as native


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Preserve previous results: output exists')
    root = Path(__file__).resolve().parents[1]
    lock_path = root / 'nexora-scale-lab/state/protocol.lock'
    with lock_path.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        rows = []
        for indexed in (False, True):
            with tempfile.TemporaryDirectory(prefix='nexora-update-probe-') as folder:
                settings = native.RocksSettings()
                settings.durability = native.DocumentDurability.Durable
                engine = native.DocEngine(folder, native.TransactionSettings(), settings)
                assert engine.create_collection('items').success
                index = native.IndexDefinition()
                index.index_name='by_username'; index.fields=['username']; index.type=native.IndexType.Unique
                assert engine.create_index('items', index).success
                for start in range(0, 5000, 500):
                    docs = [json.dumps(dict(_id=str(i), username=f'user_{i}', status='active'))
                            for i in range(start,start+500)]
                    result = engine.insert_many('items', docs)
                    assert result.success, result.error_msg
                before = engine.rocks_metrics()
                tick = time.perf_counter()
                for i in range(2000):
                    spec = native.UpdateSpec()
                    spec.set('username' if indexed else 'status', f'new_{i}')
                    result = engine.update_by_id('items', str(i), spec)
                    assert result.success, result.error_msg
                elapsed = time.perf_counter()-tick
                after = engine.rocks_metrics()
                assert engine.count('items').data == '5000'
                for i in (0, 1000, 1999):
                    result = engine.find_by_id('items', str(i))
                    assert result.success
                    document=json.loads(result.data)
                    assert document['username' if indexed else 'status'] == f'new_{i}'
                rows.append(dict(indexed_field_changed=indexed, operations=2000, seconds=elapsed,
                                 operations_per_second=2000/elapsed,
                                 logical_write_bytes=after.logical_write_bytes-before.logical_write_bytes,
                                 wal_write_bytes=after.wal_write_bytes-before.wal_write_bytes))
                assert engine.prepare_shutdown().success
                del engine
        result=dict(scope='bounded 5000-document durable diagnostic, not a 100M result', rows=rows,
                    binary_sha256=hashlib.sha256(Path(native.__file__).read_bytes()).hexdigest())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x') as output:
            json.dump(result,output,indent=2)
        print(json.dumps(result,indent=2))


if __name__=='__main__':
    main()
