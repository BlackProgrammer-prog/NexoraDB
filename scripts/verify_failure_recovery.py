#!/usr/bin/env python3
"""COR-007 bounded failure campaign; only kills owned workers and mutates fresh test stores."""
import argparse
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import tempfile

MODES = ('insert', 'json', 'bson', 'ingest', 'update', 'delete', 'update_many', 'delete_many', 'tx')
STAGES = {1: 'document', 2: 'index', 3: 'counter', 4: 'before_wal_append',
          5: 'after_wal_append', 6: 'before_wal_sync', 7: 'after_wal_sync', 8: 'after_ack'}


def run(probe, directory):
    probe = probe.resolve(strict=True)
    directory.mkdir(parents=True, exist_ok=True)
    # A unique owned run also permits repeated CTest execution without deleting evidence.
    output = Path(tempfile.mkdtemp(prefix='run-', dir=directory))
    report = {'schema': 1, 'probe': str(probe), 'probe_sha256': hashlib.sha256(probe.read_bytes()).hexdigest(),
              'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'power_loss_tested': False, 'cases': [], 'status': 'running'}

    def invoke(path, action, mode, policy, killed=False):
        result = subprocess.run([str(probe), str(path), action, mode, str(policy)],
                                capture_output=True, text=True, timeout=20)
        stem = path.name + '-' + action
        (output / (stem + '.stdout')).write_text(result.stdout)
        (output / (stem + '.stderr')).write_text(result.stderr)
        wanted = -signal.SIGKILL if killed else 0
        if result.returncode != wanted:
            raise RuntimeError(f'{stem}: exit {result.returncode}, wanted {wanted}: {result.stderr}')
        return result.stdout

    try:
        for mode in MODES:
            for stage, boundary in STAGES.items():
                if mode in ('update', 'update_many') and stage == 3:
                    continue  # No counter mutation for an update.
                path = output / f'{mode}-{boundary}-db'
                invoke(path, 'seed', mode, 0)
                stdout = invoke(path, 'crash', mode, stage, killed=True)
                ack = 'COR007_ACK' in stdout
                if ack != (stage == 8):
                    raise RuntimeError('unexpected ACK at ' + path.name)
                policy = 'before' if stage <= 4 else 'after' if stage >= 7 else 'either'
                recovered = json.loads(invoke(path, 'verify', mode, policy).splitlines()[-1])
                report['cases'].append({'operation': mode, 'boundary': boundary, 'ack': ack,
                                        'expected': policy, **recovered})
            print(f'{mode}: crash boundaries and recovery oracle passed', flush=True)

        for mode in MODES:
            path = output / f'{mode}-partial-wal-db'
            invoke(path, 'seed', mode, 0)
            stdout = invoke(path, 'partial', mode, 0, killed=True)
            if 'COR007_NACK' not in stdout or 'COR007_ACK' in stdout:
                raise RuntimeError('missing partial-write failure response')
            recovered = json.loads(invoke(path, 'reject', mode, 0).splitlines()[-1])
            report['cases'].append({'operation': mode, 'damage': 'partial_wal_append',
                                    'ack': False, **recovered})

        # Corrupt real persisted bytes after an acknowledged commit and SIGKILL.
        # Each case uses a separate store; never repair/skip a corrupt WAL to pass.
        for damage in ('wal_checksum', 'wal_truncate', 'sst_checksum'):
            path = output / (damage + '-db')
            invoke(path, 'seed', 'json', 0)
            invoke(path, 'crash', 'json', 8, killed=True)
            suffix = '*.sst' if damage.startswith('sst') else '*.log'
            candidates = sorted(p for p in path.glob(suffix) if p.stat().st_size > 16)
            if not candidates:
                raise RuntimeError('no persisted file for ' + damage)
            target = candidates[-1]
            data = bytearray(target.read_bytes())
            if damage == 'wal_truncate':
                data = data[:-1]
            else:
                data[12] ^= 0x40  # Payload/data block; retain stored CRC.
            target.write_bytes(data)
            recovered = json.loads(invoke(path, 'reject', 'json', 0).splitlines()[-1])
            report['cases'].append({'damage': damage, 'file': target.name, **recovered})
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2))
        print('COR007 report: ' + str(output / 'report.json'), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    run(args.probe, args.directory)
