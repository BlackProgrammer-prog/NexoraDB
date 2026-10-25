"""Explicit recovery of a fully verified native job stuck only in cleanup.

Never rewrites measurements. Requires ownership marker, exact process identity,
an exclusive controller lock and --apply before deleting a private store.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import time
from datetime import datetime, timezone

from scale_lab.protocol import LAB, atomic_json, binary_digest


def private_store(attempt, checkpoint):
    expected_root = LAB.resolve() / 'state/protocol'
    root = expected_root.resolve(strict=True)
    if root != expected_root:
        raise RuntimeError('Refusing a redirected protocol state root')
    supplied = Path(checkpoint['private_store'])
    store = supplied.resolve(strict=True)
    if supplied.is_symlink() or store.parent != root or not store.name.startswith('trial_'):
        raise RuntimeError('Refusing removal outside directly owned trial directory')
    owner = json.loads((store / 'owner.json').read_text())
    if owner != {'job_dir': str(attempt), 'namespace': checkpoint['namespace']}:
        raise RuntimeError('Private store ownership mismatch')
    return store


def recover(attempt, apply=False):
    attempt = Path(attempt).resolve(strict=True)
    study = attempt.parent.parent
    if not study.is_relative_to((LAB / 'results').resolve()):
        raise RuntimeError('Only benchmark result attempts can be recovered')
    checkpoint = json.loads((attempt / 'checkpoint.json').read_text())
    job = checkpoint['job']
    if job['database'] != 'nexoradb' or checkpoint['status'] != 'cleaning-up':
        raise RuntimeError('Only a Nexora cleanup-stage job can be recovered')
    ingestion = json.loads((attempt / 'ingestion.json').read_text())
    config = json.loads((attempt / 'job.json').read_text())['config']
    if ingestion['records'] != config['records']:
        raise RuntimeError('Incomplete ingestion')
    measurements = sorted(attempt.glob('cell-*/measurement.json'))
    if len(measurements) != len(job['cells']):
        raise RuntimeError('Trial set is incomplete')
    for index, path in enumerate(measurements):
        value = json.loads(path.read_text())
        cell = job['cells'][index]
        if (value['status'] != 'verified-and-restored' or value['counts'].get('failed', 0)
                or value['dataset_records'] != config['records']
                or any(value[key] != cell[key] for key in ('workload', 'clients', 'working_set'))):
            raise RuntimeError('Trial not verified or does not match the plan')
    store = private_store(attempt, checkpoint)
    workers = []
    for path in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            args = path.read_bytes().decode().split('\0')
            if '_protocol-job' in args and '--job-file' in args:
                job_file = Path(args[args.index('--job-file') + 1]).resolve()
                if job_file == attempt / 'job.json':
                    workers.append(int(path.parent.name))
        except (FileNotFoundError, ProcessLookupError):
            pass
    if len(workers) != 1:
        raise RuntimeError('Expected exactly one matching cleanup worker')
    worker = workers[0]
    parent = int(Path(f'/proc/{worker}/stat').read_text().rsplit(')', 1)[1].split()[1])
    args = Path(f'/proc/{parent}/cmdline').read_bytes().decode().split('\0')
    if 'compare' not in args or '--directory' not in args:
        raise RuntimeError('Unexpected worker parent')
    directory = Path(args[args.index('--directory')+1])
    if not directory.is_absolute():
        directory = Path(f'/proc/{parent}/cwd').resolve() / directory
    if directory.resolve() != study:
        raise RuntimeError('Parent controller belongs to another study')
    evidence = {'utc': datetime.now(timezone.utc).isoformat(), 'original_checkpoint': checkpoint,
                'measurement_sha256': {str(p.relative_to(attempt)): binary_digest(p) for p in measurements},
                'worker_pid': worker, 'controller_pid': parent, 'private_store': str(store),
                'reason': 'All trials verified; cleanup-only worker trapped in unbounded DropCollection batch.'}
    if not apply:
        return evidence
    receipt = attempt / 'cleanup-recovery.json'
    if receipt.exists():
        raise RuntimeError('Recovery evidence already exists; inspect before retrying')
    atomic_json(receipt, {**evidence, 'status': 'prepared'})
    os.kill(parent, signal.SIGINT)  # Controller terminates its own worker and records interruption.
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline and Path(f'/proc/{parent}').exists():
        time.sleep(.2)
    if Path(f'/proc/{parent}').exists() or Path(f'/proc/{worker}').exists():
        raise RuntimeError('Processes still present; no data deleted')
    with (LAB / 'state/protocol.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        private_store(attempt, checkpoint)  # Recheck ownership after all handles closed.
        for name, digest in evidence['measurement_sha256'].items():
            if binary_digest(attempt / name) != digest:
                raise RuntimeError('Measurement changed during recovery; refusing deletion')
        sizes = [p.stat().st_size for p in store.rglob('*') if p.is_file() and not p.is_symlink()]
        shutil.rmtree(store)
        checkpoint.update(status='completed', private_store_removed=True,
                          cleanup_recovered_externally=True, cleanup_recovery='cleanup-recovery.json')
        atomic_json(attempt / 'checkpoint.json', checkpoint)
        atomic_json(receipt, {**evidence, 'status': 'recovered', 'files_removed': len(sizes),
                             'logical_bytes_removed': sum(sizes),
                             'note': 'Dataset deleted after stopping cleanup worker. Measurement bytes unchanged. Not a clean original job exit.'})
        manifest = json.loads((study / 'manifest.json').read_text())
        manifest['cleanup_recoveries'] = [str(receipt.relative_to(study))]
        manifest['claims'].append('Nexora measurements verified before cleanup-only termination; private store removed externally. See recovery receipt.')
        atomic_json(study / 'manifest.json', manifest)
    from scale_lab.reporting import report
    report(study)
    return {'status': 'recovered', 'preserved_trials': len(measurements),
            'removed_private_store': str(store), 'logical_bytes_removed': sum(sizes)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('attempt', type=Path)
    parser.add_argument('--apply', action='store_true')
    options = parser.parse_args()
    print(json.dumps(recover(options.attempt, options.apply), indent=2))
