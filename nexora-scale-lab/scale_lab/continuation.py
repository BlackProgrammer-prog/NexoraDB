"""Explicit cross-revision continuation: retain completed artifacts, never fake hashes."""
import fcntl
import json
import shutil
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

from .protocol import LAB, atomic_json, binary_digest, suite_sources
from .resources import host_metadata

# Changes in order, instrumentation, resource containment and PostgreSQL tuning
# are intentional and archived separately, not represented as an identical study.
MEASUREMENT_FIELDS = (
    'records', 'seed', 'batch_size', 'workloads', 'clients', 'working_sets',
    'index_policies', 'repetitions', 'warmup_seconds', 'measurement_seconds',
    'max_operations', 'load_mode', 'target_rate', 'max_queue_delay_seconds',
    'distribution', 'zipf_theta', 'cache_mode', 'comparison_scope',
    'validation_mode', 'validation_samples',
)


def prepare_continuation(config, source_directory, destination, preserve_completed=False):
    source = Path(source_directory).resolve()
    destination = Path(destination).resolve()
    if destination == source or destination.is_relative_to(source):
        raise ValueError('Continuation must use a separate result directory')
    original = json.loads((source / 'manifest.json').read_text())
    current = asdict(config)
    for field in MEASUREMENT_FIELDS:
        if original['config'].get(field) != current[field]:
            raise ValueError(f'Cannot reuse MongoDB: measurement field differs: {field}')
    imports = []
    for job in config.jobs():
        if job['database'] != 'mongodb' and not preserve_completed:
            continue
        if job['database'] == 'nexoradb' and preserve_completed:
            for name, default in (('native_memory_budget_mb',256), ('native_block_cache_mb',0),
                                  ('native_ingestion_mode','transactional'), ('native_bson_processes',0),
                                  ('native_wal_range_sync_mb',1)):
                if original['config'].get(name,default) != current[name]:
                    raise ValueError('Cannot import Nexora under different native resource/ingestion settings; omit --preserve-completed to remeasure it')
        candidates = [old for old in original['schedule'] if all(
            old[field] == job[field] for field in ('database', 'repetition', 'index_policy', 'cells'))]
        found = None
        for old in candidates:
            for attempt in reversed(sorted((source / ('job-' + old['id'])).glob('attempt-*'))):
                checkpoint = attempt / 'checkpoint.json'
                if checkpoint.exists() and json.loads(checkpoint.read_text())['status'] == 'completed':
                    found = attempt
                    break
            if found is not None:
                break
        if found is None:
            if job['database'] != 'mongodb':
                continue
            raise ValueError('No complete matching MongoDB job; refusing to silently reload it')
        ingestion = json.loads((found / 'ingestion.json').read_text())
        if ingestion['records'] != config.records:
            raise ValueError('Archived MongoDB dataset size mismatch')
        measurements = sorted(found.glob('cell-*/measurement.json'))
        if len(measurements) != len(job['cells']):
            raise ValueError('Archived MongoDB trial set incomplete')
        for path in measurements:
            trial = json.loads(path.read_text())
            if (trial['status'] != 'verified-and-restored' or trial['counts'].get('failed', 0)
                    or trial['dataset_records'] != config.records):
                raise ValueError('Archived MongoDB trial not successfully verified')
        imports.append((job, found))
    if not imports:
        raise ValueError('Continuation requires a completed MongoDB job to preserve')
    lock_path = LAB / 'state' / 'protocol.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        destination.mkdir(parents=True, exist_ok=False)
        source_hash, files = suite_sources()
        manifest = {
            'schema_version': 1, 'status': 'prepared', 'config': current,
            'config_sha256': config.digest(), 'source_sha256': source_hash,
            'host': host_metadata(LAB.parent), 'schedule': config.jobs(),
            'preflight': {'status': 'deferred until execution; no dataset was loaded'},
            'started_utc': datetime.now(timezone.utc).isoformat(),
            'comparison_scope': config.comparison_scope,
            'unsupported': original['unsupported'],
            'claims': original['claims'] + [
                'Completed MongoDB observations imported with their original source/config provenance.',
                'Schedule stays MongoDB, NexoraDB, PostgreSQL; imported completed jobs are skipped.',
                'Imported MongoDB used the older serial loader; new jobs may use prefetch and are not identical-condition remeasurements.',
                'Cross-revision/cross-resource results are exploratory and require separate disclosure.'],
            'continuation_changes': {
                'native_memory_budget_mb': config.native_memory_budget_mb,
                'native_block_cache_mb': config.native_block_cache_mb,
                'restoration': 'bounded acknowledged chunks outside workload measurement',
                'native_ingestion': 'Validated BSON input and reused encoding/index metadata; durable atomic batches retained.',
                'load_prefetch_batches': config.load_prefetch_batches,
                'imported_mongodb': 'Older serial-loader observations, NOT remeasured with the new pipeline.'},
            'imported_jobs': [],
            'parent_study': {'directory': str(source), 'source_sha256': original['source_sha256'],
                             'config_sha256': original['config_sha256']},
        }
        for name, content in files.items():
            target = destination / 'source' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        # Preserve old measurement definitions alongside the new source snapshot.
        shutil.copytree(source / 'source', destination / 'imported-source')
        if (source / 'imported-source').is_dir():
            shutil.copytree(source / 'imported-source', destination / 'imported-ancestor-source')
        shutil.copy2(source / 'manifest.json', destination / 'parent-manifest.json')
        for job, attempt in imports:
            target = destination / ('job-' + job['id']) / 'attempt-0000'
            shutil.copytree(attempt, target)
            prior_path = attempt / 'import-provenance.json'
            prior = json.loads(prior_path.read_text()) if prior_path.exists() else None
            checksums = {}
            for path in sorted(target.rglob('*')):
                if path.is_file():
                    relative = path.relative_to(target)
                    if str(relative) == 'import-provenance.json':
                        continue  # New receipt embeds the previous receipt; don't attest overwritten bytes.
                    if binary_digest(path) != binary_digest(attempt / relative):
                        raise RuntimeError('Imported MongoDB artifact copy failed checksum verification')
                    checksums[str(relative)] = binary_digest(path)
            imported = {'job': job['id'], 'database': job['database'], 'original_attempt': str(attempt),
                        'original_source_sha256': prior['original_source_sha256'] if prior else original['source_sha256'],
                        'original_config_sha256': prior['original_config_sha256'] if prior else original['config_sha256'],
                        'previous_import': prior,
                        'artifact_sha256': checksums,
                        'note': 'Byte-preserved older observations, not remeasured with new instrumentation/limits.'}
            atomic_json(target / 'import-provenance.json', imported)
            manifest['imported_jobs'].append(imported)
            if (target / 'cleanup-recovery.json').exists():
                manifest.setdefault('cleanup_recoveries', []).append(str((target / 'cleanup-recovery.json').relative_to(destination)))
        atomic_json(destination / 'manifest.json', manifest)
    from .reporting import report
    report(destination)
    return {'status': 'prepared-not-executed', 'directory': str(destination),
            'preserved': sorted(set(j['database'] for j, _ in imports)),
            'next_databases': [j['database'] for j in config.jobs() if j['id'] not in {old['id'] for old, _ in imports}]}
