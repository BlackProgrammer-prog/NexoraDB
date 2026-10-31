"""Sequential experiment controller and isolated per-engine job worker.

No full experiment is launched by importing this module or building a plan.
"""
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

from .adapters import NexoraAdapter, PostgreSQLAdapter, MongoDBAdapter
from .dataset import DatasetConfig, PRIMARY_RECORDS, batches
from .integrity import ContentDigest, ExpectedWrites, full_validate, sampled_validate, restore
from .protocol_config import ProtocolConfig
from .resources import Sampler, host_metadata, process
from .timed import replay_writes, trial
from .progress import Dashboard, WorkerProgress, snapshot
from .loading import load_batches

LAB = Path(__file__).resolve().parents[1]


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w', encoding='utf-8') as output:
        json.dump(value, output, indent=2, allow_nan=False)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


def suite_sources():
    files = {}
    for folder in ('scale_lab', 'config', 'scripts', 'docs', 'tests'):
        for path in sorted((LAB / folder).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts and path.suffix in {'.py', '.json', '.sh', '.md', '.html', '.sql', '.conf', '.list'}:
                files[str(path.relative_to(LAB))] = path.read_bytes()
    for name in ('README.md', 'requirements.txt'):
        files[name] = (LAB / name).read_bytes()
    # Keep the ingestion implementation beside binary hashes, not only the harness.
    for folder in ('core', 'query', 'bindings', 'graph', 'NexoraDB/src', 'scripts'):
        for path in sorted((LAB.parent / folder).rglob('*')):
            if path.is_file() and path.suffix in {'.cpp', '.h', '.hpp', '.txt'}:
                files['native-source/' + str(path.relative_to(LAB.parent))] = path.read_bytes()
    digest = hashlib.sha256()
    for name, content in sorted(files.items()):
        digest.update(name.encode() + b'\0' + hashlib.sha256(content).digest())
    return digest.hexdigest(), files


def adapter_for(engine, namespace, path, config=None):
    if engine == 'nexoradb':
        return NexoraAdapter(str(path), namespace,
                             (config.native_memory_budget_mb if config else 256)*1024**2,
                             (config.native_block_cache_mb if config else 0)*1024**2,
                             config.native_ingestion_mode if config else 'transactional',
                             config.native_wal_range_sync_mb if config else 1)
    if engine == 'postgresql':
        return PostgreSQLAdapter(os.environ['SCALE_LAB_POSTGRES_DSN'], namespace)
    return MongoDBAdapter(os.environ['SCALE_LAB_MONGO_URI'], namespace)


def preflight(config):
    if sys.platform != 'linux':
        raise RuntimeError('The reproducible resource protocol currently requires Linux/WSL')
    if config.records == PRIMARY_RECORDS and os.geteuid() != 0:
        raise RuntimeError('Full profiling requires root inside WSL to read server /proc IO and PSS; use run_publication.sh through wsl -u root')
    if config.contain_memory:
        from .memory_control import ensure_group, HIGH
        if ('nexoradb' in config.databases and
                config.native_memory_budget_mb*1024**2 > HIGH-2*1024**3):
            raise RuntimeError('Native cache/memtable budget must leave >=2GiB below memory.high for process overhead')
        ensure_group('worker')
    if 'nexoradb' in config.databases:
        import nexoradb
        if not hasattr(nexoradb.DocEngine, 'iter_documents'):
            raise RuntimeError('Rebuild the native extension: streaming validation API is missing')
        if not hasattr(nexoradb.DocEngine, 'insert_many_bson'):
            raise RuntimeError('Rebuild the native extension: validated BSON insertion API is missing')
        build = nexoradb.build_info()
        if config.records == PRIMARY_RECORDS and (build.get('build_type') != 'Release'
                or any(build.get(name) for name in ('asan_enabled', 'ubsan_enabled', 'tsan_enabled'))):
            raise RuntimeError('Primary comparisons require a Release native extension without sanitizers on PYTHONPATH')
    if 'postgresql' in config.databases:
        import psycopg
        from psycopg.conninfo import conninfo_to_dict
        dsn = os.environ['SCALE_LAB_POSTGRES_DSN']
        options = conninfo_to_dict(dsn)
        if options.get('host') not in {'127.0.0.1', 'localhost', '::1'} or options.get('hostaddr'):
            raise RuntimeError('Publication profile requires explicit loopback PostgreSQL host')
        with psycopg.connect(dsn, connect_timeout=10) as connection:
            limit = int(connection.execute('SHOW max_connections').fetchone()[0])
            if limit < max(config.clients) + 10:
                raise RuntimeError(f'PostgreSQL max_connections={limit}; configure at least {max(config.clients) + 10} before this plan')
            if connection.execute('SHOW fsync').fetchone()[0] != 'on' or connection.execute('SHOW full_page_writes').fetchone()[0] != 'on':
                raise RuntimeError('PostgreSQL fsync and full_page_writes must be on')
            if config.contain_memory:
                from .memory_control import assert_postgres_contained
                backend = connection.execute('SELECT pg_backend_pid()').fetchone()[0]
                parent = int(Path(f'/proc/{backend}/stat').read_text().rsplit(')', 1)[1].split()[1])
                assert_postgres_contained(parent)
    if 'mongodb' in config.databases:
        from pymongo.uri_parser import parse_uri
        parsed = parse_uri(os.environ['SCALE_LAB_MONGO_URI'])
        if len(parsed['nodelist']) != 1 or parsed['nodelist'][0][0] not in {'127.0.0.1', 'localhost', '::1'}:
            raise RuntimeError('Publication profile requires a standalone local MongoDB deployment')
    estimate = len(json.dumps(__import__('scale_lab.dataset', fromlist=['record']).record(
        DatasetConfig(config.records, config.seed, config.batch_size), config.records - 1))) * config.records
    if shutil.disk_usage(LAB).free < estimate * 5:
        raise RuntimeError('Insufficient estimated space for one live dataset, indexes/WAL and replay journal (5x JSON heuristic)')
    return {'estimated_one_job_disk_budget_bytes': estimate * 5,
            'disk_budget_is_heuristic': True, 'available_disk_bytes': shutil.disk_usage(LAB).free,
            'scope': 'same local host, sequential jobs; combined deployment resources',
            'resource_privileges': 'root' if os.geteuid() == 0 else 'partial development metrics'}


def storage(adapter):
    if adapter.engine_name == 'nexoradb':
        return {'bytes': adapter.engine.get_disk_usage_bytes(), 'scope': 'isolated RocksDB directory including WAL/metadata'}
    if adapter.engine_name == 'postgresql':
        return {'bytes': adapter.connection().execute('SELECT pg_total_relation_size(%s::regclass)',
                                                     (adapter.namespace,)).fetchone()[0],
                'scope': 'table+indexes+TOAST; excludes cluster WAL'}
    stats = adapter.database.command('collStats', adapter.namespace)
    return {'bytes': stats['storageSize'] + stats['totalIndexSize'],
            'scope': 'collection allocated storage+indexes; excludes journal'}


def index_check(metadata):
    indexes = metadata['indexes']
    return any('username' in json.dumps(index) for index in indexes)


def run_job(config, job, job_dir):
    if job['database'] != 'nexoradb':
        return _run_job(config, job, job_dir)
    monitor = Sampler(Path(job_dir)/'job-resources.jsonl', [os.getpid()],
                      config.sample_interval_seconds, combined=True)
    try:
        with monitor:
            return _run_job(config, job, job_dir, monitor)
    finally:
        atomic_json(Path(job_dir)/'job-resources-summary.json', monitor.summary())


def _run_job(config, job, job_dir, monitor=None):
    job_dir = Path(job_dir)
    containment = None
    if config.contain_memory:
        from .memory_control import enter_group
        containment = enter_group('worker')
    dataset = DatasetConfig(config.records, config.seed, config.batch_size)
    def validate_dataset(expected_digest, writes=None, label='validating-baseline'):
        stage('counting-records', next_stage=label)
        on_count = lambda total: stage(label, total=total)
        if config.validation_mode == 'full':
            return full_validate(adapter, dataset, expected_digest, writes, progress.update, on_count)
        return sampled_validate(adapter, dataset, expected_digest, config.validation_samples, writes,
                                progress.update, on_count)
    namespace = 'scale_lab_p_' + hashlib.sha256(str(job_dir).encode()).hexdigest()[:20]
    state_root = LAB / 'state' / 'protocol'
    state_root.mkdir(parents=True, exist_ok=True)
    # Only a newly created private directory is ever removed after success.
    store_dir = Path(tempfile.mkdtemp(prefix='trial_', dir=state_root)).resolve()
    if not store_dir.is_relative_to(state_root.resolve()):
        raise RuntimeError('Private store escaped protocol state root')
    atomic_json(store_dir / 'owner.json', {'job_dir': str(job_dir), 'namespace': namespace})
    adapter = adapter_for(job['database'], namespace, store_dir / 'db', config)
    if monitor is not None: monitor.extra_callback = adapter.memory_snapshot
    state = {'job': job, 'namespace': namespace, 'private_store': str(store_dir), 'status': 'starting'}
    progress = WorkerProgress(job_dir, atomic_json)

    def stage(name, total=None, unit='records', **details):
        state.update(status=name, stage_started_utc=datetime.now(timezone.utc).isoformat(), **details)
        atomic_json(job_dir / 'checkpoint.json', state)
        progress.start(name, total, unit, cell=state.get('cell'),
                       workload=state.get('workload'), clients=state.get('clients'))
        if job['database'] == 'nexoradb' and hasattr(adapter, 'engine'):
            from .memory_diagnostics import python_memory
            from .memory_control import group_snapshot
            diagnostic = {'stage': name, 'utc': state['stage_started_utc'],
                          'allocator_maintenance': {'attempted': False, 'reason': 'observation_only_MEM002'},
                          'process': python_memory(), 'cgroup': group_snapshot('worker')}
            try:
                diagnostic['native'] = json.loads(adapter.engine.memory_report())
            except Exception as error:
                diagnostic['native_error'] = type(error).__name__
            with (job_dir / 'stage-memory.jsonl').open('a') as output:
                output.write(json.dumps(diagnostic)+'\n')

    def timed_status(value, seconds):
        progress.update(min(value['elapsed_seconds'], seconds),
                        details={**value['counts'], 'draining': value['elapsed_seconds'] >= seconds})

    stage('starting')
    prepared = False
    success = False
    try:
        stage('connecting')
        adapter.connect()
        stage('preparing-schema')
        adapter.prepare_schema()
        prepared = True
        if job['index_policy'] == 'before-load':
            stage('building-indexes')
            index_start = time.perf_counter()
            adapter.create_indexes()
            index_seconds = time.perf_counter() - index_start
        else:
            index_seconds = None
        stage('checking-engine')
        metadata = adapter.metadata()
        metadata['allocator_policy'] = {
            'scope': 'native engine and generator process' if job['database']=='nexoradb' else 'no server allocator intervention',
            'native_trim': False,
            'maintenance': 'observation_only_MEM002; no explicit malloc_trim calls',
            'MALLOC_ARENA_MAX': os.environ.get('MALLOC_ARENA_MAX'),
            'MALLOC_TRIM_THRESHOLD_': os.environ.get('MALLOC_TRIM_THRESHOLD_')}
        metadata['worker_memory_containment'] = containment
        encoded_processes=config.native_bson_processes if job['database']=='nexoradb' else 0
        metadata['load_pipeline'] = {'producer_threads': int(config.load_prefetch_batches > 0 and not encoded_processes),
                                     'producer_processes': encoded_processes,
                                     'bson_encoded_in_producers': bool(encoded_processes),
                                     'prefetch_batches': config.load_prefetch_batches,
                                     'database_writer_threads': 1}
        roots = metadata['resource_roots']
        affinities = {str(pid): sorted(os.sched_getaffinity(pid)) for pid in roots}
        expected_affinity = sorted(os.sched_getaffinity(0))
        if any(value != expected_affinity for value in affinities.values()):
            raise RuntimeError('Server and generator CPU affinity differ; normalize before comparison')
        metadata['resource_affinity'] = affinities
        if adapter.engine_name == 'nexoradb':
            metadata['native_binary_sha256'] = binary_digest(Path(metadata['native_binary']))
            binary_copy = job_dir/'native-binary'/Path(metadata['native_binary']).name
            binary_copy.parent.mkdir()
            shutil.copyfile(metadata['native_binary'], binary_copy)
            if binary_digest(binary_copy) != metadata['native_binary_sha256']:
                raise RuntimeError('Native binary changed during evidence copy')
            metadata['native_binary_artifact'] = str(binary_copy)
            if config.records == PRIMARY_RECORDS and metadata['native_build'].get('build_type') != 'Release':
                raise RuntimeError('Primary comparisons require a Release native build, not Debug/sanitized artifacts')
        if adapter.engine_name == 'mongodb':
            metadata['server_config'] = Path('/etc/mongod.conf').read_text()
            if adapter.client.admin.command('hello').get('setName'):
                raise RuntimeError('This profile specifies standalone MongoDB; a replica set is a different durability scenario')
        for pid in roots:
            observed = process(pid)
            if config.records == PRIMARY_RECORDS and ('io_error' in observed or 'pss_error' in observed):
                raise RuntimeError('Server IO/PSS is unreadable; resource profiling cannot be certified')
        atomic_json(job_dir / 'engine.json', metadata)
        stage('loading', total=config.records)
        digest = ContentDigest()
        load_started = time.perf_counter()
        producer_wait_seconds = batch_ack_seconds = 0.0
        batch_count = 0
        if encoded_processes:
            from .encoded_loading import encoded_batches
            pipeline=encoded_batches(dataset,encoded_processes)
        else:
            pipeline=load_batches(dataset,config.load_prefetch_batches)
        with Sampler(job_dir / 'load-resources.jsonl', roots, config.sample_interval_seconds,
                     combined=adapter.engine_name == 'nexoradb') as samples, \
                pipeline as prepared_batches:
            last_progress = load_started
            previous_records = 0
            def log_load_metrics(now, records):
                nonlocal previous_records, last_progress
                diagnostic = {'event': 'loading', 'job': job['id'], 'records': records,
                              'target': config.records, 'elapsed_seconds': now-load_started,
                              'recent_per_second': (records-previous_records)/max(1e-9,now-last_progress),
                              'average_per_second': records/max(1e-9,now-load_started),
                              'producer_wait_seconds': producer_wait_seconds,
                              'batch_ack_seconds': batch_ack_seconds, 'batches': batch_count}
                try:
                    diagnostic['engine_metrics'] = adapter.load_metrics()
                except Exception as error:
                    diagnostic['engine_metrics_error'] = type(error).__name__
                with (job_dir / 'load-engine.jsonl').open('a') as output:
                    output.write(json.dumps(diagnostic)+'\n')
                print(json.dumps(diagnostic), flush=True)
                progress.update(records, engine_metrics=diagnostic.get('engine_metrics', {}),
                                batch_ack_seconds=batch_ack_seconds,
                                producer_wait_seconds=producer_wait_seconds)
                previous_records, last_progress = records, now

            prepared_iterator = iter(prepared_batches)
            while True:
                wait_started = time.perf_counter()
                try:
                    batch, prepared_digest = next(prepared_iterator)
                except StopIteration:
                    break
                producer_wait_seconds += time.perf_counter() - wait_started
                ack_started = time.perf_counter()
                if encoded_processes:
                    adapter.batch_insert_bson(batch)
                else:
                    adapter.batch_insert(batch)
                last_ack_seconds=time.perf_counter()-ack_started
                batch_ack_seconds += last_ack_seconds
                batch_count += 1
                digest = prepared_digest  # Progress advances only after durable acknowledgment.
                progress.update(digest.count,last_batch_ack_ms=last_ack_seconds*1000)
                if time.perf_counter() - last_progress >= 5:
                    if shutil.disk_usage(LAB).free < 2 * 1024 ** 3:
                        raise RuntimeError('Disk pressure: load stopped to preserve checkpoint/report space')
                    if config.contain_memory:
                        from .memory_control import host_memory
                        if host_memory().get('MemAvailable', 0) < 1024**3:
                            raise RuntimeError('Low Linux MemAvailable: stopping before exhausting guest memory')
                    log_load_metrics(time.perf_counter(),digest.count)
            if previous_records != digest.count:
                log_load_metrics(time.perf_counter(),digest.count)
        elapsed = time.perf_counter() - load_started
        ingestion = {'records': digest.count, 'seconds': elapsed, 'records_per_second': digest.count / elapsed,
                     'pipeline_timing': {'producer_wait_seconds': producer_wait_seconds,
                                         'batch_ack_seconds': batch_ack_seconds, 'batches': batch_count,
                                         'note': 'ack includes BSON/client encoding and database acknowledgment; producer work overlaps'},
                     'resources': samples.summary(), 'includes': 'generation, serialization, client transfer, acknowledgment and streaming checksum',
                     'baseline_digest': digest.result(), 'index_policy': job['index_policy']}
        if job['index_policy'] == 'after-load':
            stage('building-indexes')
            index_start = time.perf_counter()
            adapter.create_indexes()
            index_seconds = time.perf_counter() - index_start
        ingestion['index_build_seconds'] = index_seconds
        if hasattr(adapter, 'optimize'):
            stage('analyzing-statistics')
            adapter.optimize()
        stage('checking-index-plans')
        metadata = {**metadata, 'indexes': adapter.metadata()['indexes']}
        if not index_check(metadata):
            raise RuntimeError('Required username secondary index was not verified')
        metadata['username_lookup_plan'] = adapter.lookup_plan()
        if config.records == PRIMARY_RECORDS and not metadata['username_lookup_plan']['uses_secondary_index']:
            raise RuntimeError('Indexed lookup comparison refused: optimizer did not choose a secondary index')
        if job['database'] == 'nexoradb' and any(i['state'] != 'IndexState.Ready' for i in metadata['indexes']):
            raise RuntimeError('NexoraDB index is not ready')
        atomic_json(job_dir / 'engine.json', metadata)
        ingestion['validation'] = validate_dataset(digest.result())
        ingestion['storage'] = storage(adapter)
        atomic_json(job_dir / 'ingestion.json', ingestion)
        for cell_id, cell in enumerate(job['cells']):
            cell_dir = job_dir / f'cell-{cell_id:04d}'
            cell_dir.mkdir()
            stage('warming', total=config.warmup_seconds, unit='seconds', cell=cell_id,
                  workload=cell['workload'], clients=cell['clients'])
            seed = config.seed + (job['repetition'] - 1) * 1_000_003
            # Read-only conditioning preserves exactly the baseline logical state
            # at measurement start, including for write workloads A and B.
            if config.warmup_seconds:
                warmup = trial(adapter, dataset, config,
                               'indexed-read' if cell['workload'] == 'indexed-read' else 'ycsb-c',
                               cell['clients'], cell['working_set'], seed, config.warmup_seconds,
                               status_callback=lambda value: timed_status(value, config.warmup_seconds))
                atomic_json(cell_dir / 'warmup.json', warmup)
                if warmup['counts'].get('failed', 0):
                    raise RuntimeError('Warm-up failed; measured trial was not started')
            stage('measuring', total=config.measurement_seconds, unit='seconds')
            samples = Sampler(cell_dir / 'resources.jsonl', roots, config.sample_interval_seconds,
                              combined=adapter.engine_name == 'nexoradb')
            measured = trial(adapter, dataset, config, cell['workload'], cell['clients'],
                             cell['working_set'], seed, config.measurement_seconds,
                             config.max_operations, monitor=samples,
                             progress_callback=lambda value: atomic_json(cell_dir / 'in-progress.json', value),
                             status_callback=lambda value: timed_status(value, config.measurement_seconds))
            measured.update(database=job['database'], repetition=job['repetition'],
                            index_policy=job['index_policy'], resources=samples.summary(),
                            dataset_records=config.records, baseline_digest=digest.result(),
                            status='measured-not-yet-validated')
            atomic_json(cell_dir / 'measurement.json', measured)
            if not measured['counts'].get('attempted', 0) or measured['counts'].get('failed', 0):
                raise RuntimeError('Measured trial failed; all failures are retained in measurement.json')
            stage('replaying-writes', total=measured['counts']['attempted'], unit='operations')
            writes = ExpectedWrites(cell_dir / 'expected-writes.sqlite')
            try:
                replay_writes(writes, dataset, config, measured, progress.update)
                measured['post_validation'] = validate_dataset(digest.result(), writes, 'validating-results')
                modified = writes.connection.execute('SELECT count(*) FROM writes').fetchone()[0]
                stage('restoring-records', total=modified)
                measured['restoration'] = restore(adapter, dataset, writes, progress.update)
                measured['restored_validation'] = validate_dataset(digest.result(), label='verifying-restored')
                if config.validation_mode != 'full':
                    measured['restoration']['policy'] = 'restore every replayed mutated key, then sampled validation'
            finally:
                writes.close()
            measured['status'] = 'verified-and-restored'
            atomic_json(cell_dir / 'measurement.json', measured)
            print(json.dumps({'event': 'trial-complete', 'job': job['id'], 'cell': cell_id,
                              'database': job['database'], **cell}), flush=True)
        stage('cleaning-up')
        success = True
    except BaseException as error:
        state.update(failed_stage=state['status'], status='failed', error_type=type(error).__name__)
        if job['database'] == 'nexoradb':
            state['error_message'] = str(error)[:4000]
        # Preserve partial stores and checkpoints. Never silently retry a write
        # with an unknown outcome or append to an interrupted load.
        atomic_json(job_dir / 'checkpoint.json', state)
        raise
    finally:
        try:
            if success and prepared and job['database'] != 'nexoradb':
                adapter.cleanup()
            if success and job['database'] == 'nexoradb':
                state['cleanup_policy'] = 'close engine, remove owned private store; no logical collection drop'
                stage('closing-engine')
            from .memory_control import cleanup_headroom, group_snapshot
            # Measurements and restoration have ended. Keep the hard cap intact;
            # scoped relief is explicitly recorded and is never benchmark time.
            with cleanup_headroom(config.contain_memory and job['database'] == 'nexoradb'):
                if job['database'] == 'nexoradb':
                    atomic_json(job_dir / 'cleanup-memory.json', {
                        'policy': 'temporary soft threshold = hard cap, restored on exit',
                        'enabled': config.contain_memory, 'before_close': group_snapshot('worker')})
                adapter.disconnect()
                if job['database'] == 'nexoradb':
                    atomic_json(job_dir/'close.json', adapter.close_report)
                    atomic_json(job_dir/'cleanup-memory.json', {
                        'policy': 'temporary soft threshold = hard cap, restored on exit',
                        'enabled': config.contain_memory, 'after_close': group_snapshot('worker'),
                        'before_close': json.loads((job_dir/'cleanup-memory.json').read_text())['before_close']})
            if job['database'] == 'nexoradb':
                cleanup = json.loads((job_dir/'cleanup-memory.json').read_text())
                cleanup['after_restore'] = group_snapshot('worker')
                atomic_json(job_dir/'cleanup-memory.json', cleanup)
        except BaseException as error:
            success = False
            state['cleanup_error_type'] = type(error).__name__
            if job['database'] == 'nexoradb':
                state['cleanup_error_message'] = str(error)[:4000]
                if hasattr(adapter, 'close_report'): atomic_json(job_dir/'close.json', adapter.close_report)
            if 'failed_stage' not in state:
                state.update(status='cleanup-failed', error_type=type(error).__name__)
            atomic_json(job_dir / 'checkpoint.json', state)
            if 'failed_stage' not in state:
                raise
        else:
            if success:
                stage('removing-private-store')
                # Resolve again and require the ownership marker before removal.
                owner = json.loads((store_dir / 'owner.json').read_text())
                if (store_dir.parent == state_root.resolve() and not store_dir.is_symlink()
                        and owner == {'job_dir': str(job_dir), 'namespace': namespace}):
                    shutil.rmtree(store_dir)
                    state['private_store_removed'] = True
                else:
                    raise RuntimeError('Private store ownership check failed; directory retained')
                state['status'] = 'completed'
                progress.start('completed', total=1, unit='job')
                progress.update(1, force=True)
                atomic_json(job_dir / 'checkpoint.json', state)


def binary_digest(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def execute(config, destination, allow_large=False, resume=False):
    if config.records >= PRIMARY_RECORDS and not allow_large:
        raise ValueError('Full-size execution requires explicit --allow-large')
    checks = preflight(config)
    destination = Path(destination).resolve()
    source_hash, files = suite_sources()
    if resume:
        manifest = json.loads((destination / 'manifest.json').read_text())
        if manifest['config_sha256'] != config.digest() or manifest['source_sha256'] != source_hash:
            raise ValueError('Resume refused: config/source differ from the recorded experiment')
    else:
        destination.mkdir(parents=True, exist_ok=False)
        manifest = {'schema_version': 1, 'config': asdict(config), 'config_sha256': config.digest(),
                    'source_sha256': source_hash, 'host': host_metadata(LAB.parent),
                    'schedule': config.jobs(), 'preflight': checks,
                    'started_utc': datetime.now(timezone.utc).isoformat(),
                    'status': 'running', 'comparison_scope': config.comparison_scope,
                    'unsupported': ['cold-cache verification', 'YCSB D/E/F', 'join/range/aggregate workloads',
                                    'per-engine network byte attribution', 'isolated native server resource attribution'],
                    'claims': ['local deployment end-to-end comparison, not engine-only ranking',
                               'uniform/Zipf access within disjoint client key partitions',
                               'same unrestricted local host; vendor cache settings recorded, not claimed equal']}
        for name, content in files.items():
            target = destination / 'source' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
    manifest['status'] = 'running'
    atomic_json(destination / 'manifest.json', manifest)
    # Advisory lock lives in a stable file; never unlink a held lock. Prevents
    # concurrent protocol controllers even when output directories differ.
    import fcntl
    lock_path = LAB / 'state' / 'protocol.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            manifest['status'] = 'not-started-lock-busy'
            atomic_json(destination / 'manifest.json', manifest)
            raise RuntimeError('Another protocol controller is already running; competing trials are forbidden')
        display = Dashboard(destination)
        display.event({'event': 'run-started', 'directory': str(destination),
                       'job_timeout_seconds': config.job_timeout_seconds})
        completed_trials = 0
        total_trials = sum(len(job['cells']) for job in manifest['schedule'])
        try:
            for job_index, job in enumerate(manifest['schedule']):
                base = destination / ('job-' + job['id'])
                attempts = sorted(base.glob('attempt-*')) if base.exists() else []
                if attempts:
                    latest = attempts[-1] / 'checkpoint.json'
                    if latest.exists() and json.loads(latest.read_text())['status'] == 'completed':
                        completed_trials += len(job['cells'])
                        continue
                preflight(config)  # Recheck disk headroom before every new load.
                attempt = base / f'attempt-{len(attempts):04d}'
                attempt.mkdir(parents=True)
                job_file = attempt / 'job.json'
                atomic_json(job_file, {'config': asdict(config), 'job': job})
                with (attempt / 'stdout.jsonl').open('x') as stdout, (attempt / 'stderr.log').open('x') as stderr:
                    child = subprocess.Popen([sys.executable, '-m', 'scale_lab', '_protocol-job',
                                              '--job-file', str(job_file)], stdout=stdout, stderr=stderr,
                                             start_new_session=True, cwd=LAB)
                    try:
                        deadline = (None if config.job_timeout_seconds is None
                                    else time.monotonic() + config.job_timeout_seconds)
                        while True:
                            value = snapshot(attempt, job, job_index, len(manifest['schedule']),
                                             completed_trials, total_trials)
                            atomic_json(destination / 'live.json', value)
                            display.update(value)
                            try:
                                code = child.wait(timeout=(1 if deadline is None
                                    else min(1, max(.001, deadline - time.monotonic()))))
                                break
                            except subprocess.TimeoutExpired:
                                if deadline is not None and time.monotonic() >= deadline:
                                    raise
                    except BaseException:
                        os.killpg(child.pid, signal.SIGTERM)
                        try:
                            child.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            os.killpg(child.pid, signal.SIGKILL)
                            child.wait()
                        raise
                if code:
                    raise RuntimeError(f'Job {job["id"]} failed; inspect {attempt}/checkpoint.json and stderr.log')
                value = snapshot(attempt, job, job_index + 1, len(manifest['schedule']),
                                 completed_trials, total_trials)
                atomic_json(destination / 'live.json', value)
                display.update(value)
                display.event({'event': 'job-complete', 'job': job['id'], 'database': job['database']})
                completed_trials += len(job['cells'])
            manifest['status'] = 'completed'
            manifest['completed_utc'] = datetime.now(timezone.utc).isoformat()
        except BaseException as error:
            manifest.update(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed',
                            error_type=type(error).__name__)
            raise
        finally:
            atomic_json(destination / 'manifest.json', manifest)
            display.event({'event': 'run-' + manifest['status'], 'directory': str(destination)})
            display.close()
            from .reporting import report
            report(destination)
    return {'status': manifest['status'], 'directory': str(destination),
            'report': str(destination / 'report.html')}
