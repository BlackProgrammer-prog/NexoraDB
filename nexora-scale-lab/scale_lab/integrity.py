"""Full streaming integrity checks and disk-backed expected write state."""
import hashlib
import sqlite3
import random
from dataclasses import dataclass

from .dataset import canonical, record

MODULUS = 1 << 256


@dataclass
class ContentDigest:
    count: int = 0
    total: int = 0
    xor: int = 0

    def add(self, value):
        hashed = int.from_bytes(hashlib.sha256(canonical(value)).digest(), 'big')
        self.count += 1
        self.total = (self.total + hashed) % MODULUS
        self.xor ^= hashed

    def result(self):
        return {'algorithm': 'sha256-sum256-xor256-v1', 'records': self.count,
                'sum256': f'{self.total:064x}', 'xor256': f'{self.xor:064x}'}


class ExpectedWrites:
    """Built AFTER measurement by replaying each successful worker's schedule.

    Workers own disjoint key partitions, so the final write per key is defined
    without reconstructing a cross-thread execution order. Disk, not RAM, bounds
    the number of distinct mutated keys.
    """
    def __init__(self, path):
        self.connection = sqlite3.connect(path)
        self.connection.execute('PRAGMA cache_size=-8192')
        self.connection.execute('CREATE TABLE IF NOT EXISTS writes (id TEXT PRIMARY KEY, status TEXT NOT NULL)')

    def put(self, key, status):
        self.connection.execute('INSERT INTO writes VALUES (?,?) ON CONFLICT(id) DO UPDATE SET status=excluded.status',
                                (key, status))

    def get(self, key, default):
        row = self.connection.execute('SELECT status FROM writes WHERE id=?', (key,)).fetchone()
        return row[0] if row else default

    def close(self):
        self.connection.close()


def full_validate(adapter, config, expected_digest=None, writes=None, progress=None, on_count=None):
    actual = ContentDigest()
    reference = ContentDigest()
    count = adapter.count()
    if count != config.records:
        raise RuntimeError(f'Integrity count mismatch: {count} != {config.records}')
    if on_count is not None:
        on_count(config.records)
    for value in adapter.iter_records(config.batch_size):
        key = value.get('id')
        if not isinstance(key, str) or len(key) != 12 or not key.isascii() or not key.isdigit():
            raise RuntimeError('Integrity: malformed primary key')
        index = int(key)
        baseline = record(config, index)
        expected = dict(baseline)
        if key != expected['id']:
            raise RuntimeError('Integrity: noncanonical primary key')
        if writes is not None:
            expected['status'] = writes.get(key, expected['status'])
        if value != expected:
            raise RuntimeError(f'Integrity mismatch at {key}')
        actual.add(value)
        reference.add(baseline)
        if progress is not None and (actual.count % config.batch_size == 0 or actual.count == config.records):
            progress(actual.count)
    # Together with unique primary keys and a valid key range, N valid records
    # proves coverage of the exact N-key dataset without an in-memory ID set.
    if actual.count != config.records:
        raise RuntimeError('Integrity: streaming count mismatch')
    if expected_digest is not None and reference.result() != expected_digest:
        raise RuntimeError('Integrity: baseline dataset fingerprint mismatch')
    return {'mode': 'full-stream-every-field', 'actual': actual.result(),
            'baseline': reference.result(), 'verified_records': actual.count}


def restore(adapter, config, writes, progress=None):
    restored = 0
    pending = []
    chunk_size = min(config.batch_size, 1000)
    for (key,) in writes.connection.execute('SELECT id FROM writes ORDER BY id'):
        pending.append((key, record(config, int(key))['status']))
        if len(pending) >= chunk_size:
            adapter.restore_batch(pending)
            restored += len(pending)
            pending.clear()
            if progress is not None:
                progress(restored)
    if pending:
        adapter.restore_batch(pending)
        restored += len(pending)
    if progress is not None:
        progress(restored)
    return {'restored_keys': restored, 'batch_size': chunk_size,
            'policy': 'bounded acknowledged restore batches outside measurement, then full validation'}


def sampled_validate(adapter, config, expected_digest, samples=1000, writes=None, progress=None, on_count=None):
    """Exploratory checks only. Never represent a sample as a stored-data checksum."""
    count = adapter.count()
    if count != config.records:
        raise RuntimeError(f'Integrity count mismatch: {count} != {config.records}')
    rng = random.Random(config.seed)
    indices = {0, config.records - 1} | set(rng.sample(range(config.records), min(samples, config.records)))
    if on_count is not None:
        on_count(len(indices))
    for checked, index in enumerate(sorted(indices), 1):
        expected = record(config, index)
        if writes is not None:
            expected['status'] = writes.get(expected['id'], expected['status'])
        if adapter.read_by_id(expected['id']) != expected:
            raise RuntimeError(f'Sampled integrity mismatch at {index}')
        if progress is not None:
            progress(checked)
    return {'mode': 'sampled-every-field', 'record_count': count, 'verified_records': len(indices),
            'baseline': expected_digest, 'stored_dataset_digest_verified': False,
            'limitation': 'Full stored-data checksum and integrity of unsampled records were not verified.'}
