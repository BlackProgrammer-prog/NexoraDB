"""Dataset tools and explicit database smoke trials."""
import argparse
import json
import shutil
import os
from pathlib import Path
from dataclasses import asdict

from .dataset import DatasetConfig, PRIMARY_RECORDS, canonical, fingerprint, record


def main():
    parser = argparse.ArgumentParser(prog='python -m scale_lab')
    parser.add_argument('command', choices=('plan', 'dataset', 'prepare', 'load', 'run', 'protocol-plan', 'compare', 'report', 'prepare-continuation', '_protocol-job'))
    parser.add_argument('--records', type=int, default=PRIMARY_RECORDS)
    parser.add_argument('--seed', type=int, default=2026)
    parser.add_argument('--batch-size', type=int, default=1000)
    parser.add_argument('--allow-large', action='store_true')
    parser.add_argument('--database', choices=('nexoradb', 'postgresql', 'mongodb'), default='nexoradb')
    parser.add_argument('--namespace', default='scale_lab_records')
    parser.add_argument('--path', default='state/nexoradb')
    parser.add_argument('--workload', choices=('ycsb-a', 'ycsb-b', 'ycsb-c'), default='ycsb-c')
    parser.add_argument('--clients', type=int, default=1)
    parser.add_argument('--operations', type=int, default=1000)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--config', type=Path, default=Path(__file__).resolve().parents[1] / 'config' / 'publication.json')
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--job-file', type=Path)
    parser.add_argument('--source-directory', type=Path)
    parser.add_argument('--preserve-completed', action='store_true',
                        help='Explicitly import every complete verified engine job, not only MongoDB')
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('output already exists; choose a new file to preserve earlier results')
    try:
        config = DatasetConfig(args.records, args.seed, args.batch_size)
    except ValueError as error:
        parser.error(str(error))
    if args.command in {'protocol-plan', 'compare', 'report', 'prepare-continuation', '_protocol-job'}:
        from .protocol_config import ProtocolConfig
        if args.command == 'report':
            from .reporting import report
            if args.directory is None:
                parser.error('report requires --directory')
            result = report(args.directory)
        elif args.command == '_protocol-job':
            from .protocol import run_job
            if args.job_file is None:
                parser.error('internal job command requires --job-file')
            try:
                payload = json.loads(args.job_file.read_text())
                run_job(ProtocolConfig(**payload['config']), payload['job'], args.job_file.parent)
                return
            except BaseException as error:
                import sys
                print(json.dumps({'event': 'job-error', 'type': type(error).__name__,
                                  'sqlstate': getattr(error, 'sqlstate', None)}), file=sys.stderr)
                raise SystemExit(1)
        else:
            protocol = ProtocolConfig.read(args.config)
            if args.command == 'prepare-continuation':
                if args.directory is None or args.source_directory is None:
                    parser.error('prepare-continuation requires --source-directory and a new --directory')
                from .continuation import prepare_continuation
                result = prepare_continuation(protocol, args.source_directory, args.directory, args.preserve_completed)
            elif args.command == 'protocol-plan':
                result = protocol.plan()
            else:
                from .protocol import execute
                if args.directory is None:
                    parser.error('compare requires a new --directory (or an existing one with --resume)')
                result = execute(protocol, args.directory, args.allow_large, args.resume)
    elif args.command in {'prepare', 'load', 'run'}:
        from .adapters import NexoraAdapter, PostgreSQLAdapter, MongoDBAdapter
        from .runner import load, run
        if config.records >= PRIMARY_RECORDS and not args.allow_large:
            parser.error('100M database operations require explicit --allow-large authorization')
        if args.clients <= 0 or args.operations <= 0:
            parser.error('clients and operations must be positive')
        if args.database == 'nexoradb':
            adapter = NexoraAdapter(args.path, args.namespace)
        elif args.database == 'postgresql':
            adapter = PostgreSQLAdapter(os.environ['SCALE_LAB_POSTGRES_DSN'], args.namespace)
        else:
            adapter = MongoDBAdapter(os.environ['SCALE_LAB_MONGO_URI'], args.namespace)
        try:
            adapter.connect()
            if args.command == 'prepare':
                adapter.prepare_schema()
                adapter.create_indexes()
                result = {'prepared': args.namespace, 'database': args.database}
            elif args.command == 'load':
                result = load(adapter, config)
            else:
                result = run(adapter, config, args.workload, args.clients, args.operations)
            result['dataset_config'] = asdict(config)
            result['publication_eligible'] = False
        finally:
            adapter.disconnect()
    elif args.command == 'dataset':
        if config.records >= PRIMARY_RECORDS and not args.allow_large:
            parser.error('large generation requires explicit --allow-large')
        result = fingerprint(config)
    else:
        sample = min(config.records, 1000)
        mean_bytes = sum(len(canonical(record(config, i))) for i in range(sample)) / sample
        logical_bytes = int(mean_bytes * config.records)
        free = shutil.disk_usage('.').free
        result = {'config': asdict(config), 'sample_records': sample,
                  'estimated_logical_json_bytes': logical_bytes,
                  'planning_disk_budget_bytes': logical_bytes * 4,
                  'available_disk_bytes': free,
                  'disk_budget_available': free >= logical_bytes * 4,
                  'notes': ['4x is a planning heuristic per engine, not measured storage.',
                            'Index, WAL, compaction and restore copies require measurement.',
                            'Python object memory exceeds serialized batch size.',
                            'No database benchmark has been executed.']}
    encoded = json.dumps(result, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x', encoding='utf-8') as output:
            output.write(encoded + '\n')
    print(encoded)
    if args.command == 'run' and (result['counts'].get('failed', 0) or result['record_count_mismatch']):
        raise SystemExit(1)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        import sys
        print('Stopped by Ctrl+C. Results/checkpoints and incomplete datasets are retained.', file=sys.stderr)
        raise SystemExit(130)
