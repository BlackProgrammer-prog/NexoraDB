"""Versioned random-access records and bounded-memory batch generation."""
import hashlib
import json
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Iterator

PRIMARY_RECORDS = 100_000_000
DATASET_VERSION = 1


@dataclass(frozen=True)
class DatasetConfig:
    records: int = PRIMARY_RECORDS
    seed: int = 2026
    batch_size: int = 1000

    def __post_init__(self):
        for name in ('records', 'batch_size'):
            if type(getattr(self, name)) is not int or getattr(self, name) <= 0:
                raise ValueError(f'{name} must be a positive integer')
        if type(self.seed) is not int:
            raise ValueError('seed must be an integer')
        if self.records > 10**12:
            raise ValueError('records exceeds the version 1 key space')


def record(config: DatasetConfig, index: int) -> dict:
    if type(index) is not int or not 0 <= index < config.records:
        raise ValueError('index outside dataset')
    digest = hashlib.sha256(f'v{DATASET_VERSION}:{config.seed}:{index}'.encode()).digest()
    timestamp = datetime(2026, 1, 1, tzinfo=timezone.utc) + timedelta(
        seconds=int.from_bytes(digest[8:12], 'big') % (365 * 86400))
    return {
        'id': f'{index:012d}', 'username': f'user_{index}',
        'age': 18 + digest[0] % 73,
        'country': ('DE', 'US', 'IR', 'GB', 'JP', 'BR', 'IN', 'FR')[digest[1] % 8],
        'status': 'active' if digest[2] < 205 else 'inactive',
        'balance_cents': int.from_bytes(digest[3:7], 'big') % 10_000_000,
        'created_at': timestamp.isoformat().replace('+00:00', 'Z'),
        'tags': ['premium' if digest[7] < 64 else 'standard', 'mobile'],
        'metadata': {'device': 'android' if digest[12] < 128 else 'ios',
                     'region': None if digest[13] < 16 else 'global'},
    }


def canonical(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(',', ':'),
                       ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')


def batches(config: DatasetConfig, start: int = 0, stop: int | None = None) -> Iterator[list[dict]]:
    stop = config.records if stop is None else stop
    if not 0 <= start <= stop <= config.records:
        raise ValueError('invalid partition bounds')
    for offset in range(start, stop, config.batch_size):
        yield [record(config, i) for i in range(offset, min(offset + config.batch_size, stop))]


def fingerprint(config: DatasetConfig) -> dict:
    digest = hashlib.sha256()
    count = total_bytes = 0
    for batch in batches(config):
        for value in batch:
            encoded = canonical(value)
            digest.update(encoded)
            count += 1
            total_bytes += len(encoded)
    return {'dataset_version': DATASET_VERSION, 'records': count, 'seed': config.seed,
            'canonical_bytes': total_bytes, 'sha256': digest.hexdigest()}
