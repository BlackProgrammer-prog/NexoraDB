"""Small, local A/B of cursor encoding; excludes HTTP/TLS and 100M ingestion."""
import argparse
import json
from pathlib import Path
import statistics
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rows', type=int, default=10000)
    parser.add_argument('--repetitions', type=int, default=5)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.rows <= 100000 or not 3 <= args.repetitions <= 20:
        parser.error('rows 1..100000; repetitions 3..20')
    if args.output.exists():
        parser.error('output already exists; preserve earlier evidence')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import nexoradb
    with tempfile.TemporaryDirectory(prefix='nexora-cursor-encoding-') as directory:
        engine = nexoradb.DocEngine(directory)
        assert engine.create_collection('items').success
        for first in range(0, args.rows, 1000):
            batch = [json.dumps({'_id': str(i), 'text': 'x' * 128, 'nested': [i, True, None]})
                     for i in range(first, min(first + 1000, args.rows))]
            assert engine.insert_many('items', batch).success

        def scan(raw):
            count, size = 0, 0
            with engine.iter_documents('items', page_size=128, max_page_bytes=1024 * 1024) as stream:
                if raw:
                    while True:
                        chunk, records = stream.next_ndjson_batch(128)
                        if not records:
                            break
                        count += records
                        size += len(chunk)
                else:
                    for item in stream:
                        chunk = (json.dumps({'document': item}, ensure_ascii=False,
                                            separators=(',', ':')) + '\n').encode('utf-8')
                        count += 1
                        size += len(chunk)
            assert count == args.rows
            return size

        timings = {'python_roundtrip_ms': [], 'native_ndjson_ms': []}
        for raw in (False, True):
            scan(raw)
        for repeat in range(args.repetitions):
            # Alternate ordering to reduce cache/order bias.
            for raw in ((False, True) if repeat % 2 == 0 else (True, False)):
                start = time.perf_counter()
                scan(raw)
                timings['native_ndjson_ms' if raw else 'python_roundtrip_ms'].append(
                    (time.perf_counter() - start) * 1000)
        report = {'rows': args.rows, 'repetitions': args.repetitions,
                  'scope': 'warm local scan+encoding only; no network/TLS/auth; not ingestion',
                  'samples': timings,
                  'medians': {key: statistics.median(values) for key, values in timings.items()}}
        assert engine.prepare_shutdown().success
        del engine
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(report, output, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
