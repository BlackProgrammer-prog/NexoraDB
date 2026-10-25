"""Strict, versioned execution plans; dry planning has no database side effects."""
import hashlib
import json
import math
import random
from dataclasses import asdict, dataclass, field

from .dataset import DatasetConfig, PRIMARY_RECORDS


@dataclass(frozen=True)
class ProtocolConfig:
    records: int = PRIMARY_RECORDS
    seed: int = 2026
    batch_size: int = 1000
    databases: list = field(default_factory=lambda: ['nexoradb', 'postgresql', 'mongodb'])
    workloads: list = field(default_factory=lambda: ['ycsb-a', 'ycsb-b', 'ycsb-c', 'indexed-read'])
    clients: list = field(default_factory=lambda: [1, 10, 50, 100, 250, 500])
    working_sets: list = field(default_factory=lambda: [1.0])
    index_policies: list = field(default_factory=lambda: ['before-load', 'after-load'])
    repetitions: int = 5
    warmup_seconds: float = 60
    measurement_seconds: float = 300
    max_operations: int | None = None
    load_mode: str = 'closed-loop'
    target_rate: float | None = None
    max_queue_delay_seconds: float = 1.0
    distribution: str = 'uniform'
    zipf_theta: float = 0.99
    cache_mode: str = 'warm-conditioned'
    comparison_scope: str = 'local-deployments'
    sample_interval_seconds: float = 1.0
    job_timeout_seconds: float | None = 1209600
    validation_mode: str = 'full'
    validation_samples: int = 1000
    database_order: str = 'randomized'
    contain_memory: bool = False
    load_prefetch_batches: int = 0
    native_memory_budget_mb: int = 256
    native_block_cache_mb: int = 0
    native_ingestion_mode: str = 'transactional'
    native_bson_processes: int = 0
    native_wal_range_sync_mb: int = 1

    def __post_init__(self):
        DatasetConfig(self.records, self.seed, self.batch_size)
        if self.native_ingestion_mode not in {'transactional','exclusive-bulk'}:
            raise ValueError('Invalid native ingestion mode')
        if type(self.native_bson_processes) is not int or self.native_bson_processes not in {0,1,2,4}:
            raise ValueError('native_bson_processes must be 0,1,2,4')
        if type(self.native_wal_range_sync_mb) is not int or self.native_wal_range_sync_mb not in {0,1,4}:
            raise ValueError('Invalid native WAL range sync setting')
        if self.native_ingestion_mode=='exclusive-bulk' and self.batch_size>10000:
            raise ValueError('Exclusive ingestion requires batches <=10000')
        if (type(self.native_memory_budget_mb) is not int or not 4 <= self.native_memory_budget_mb <= 16384
                or type(self.native_block_cache_mb) is not int
                or not 0 <= self.native_block_cache_mb < self.native_memory_budget_mb):
            raise ValueError('Invalid native cache/memtable budget in MiB')
        if type(self.load_prefetch_batches) is not int or not 0 <= self.load_prefetch_batches <= 8:
            raise ValueError('load_prefetch_batches must be in [0, 8]')
        if self.database_order not in {'randomized', 'listed'} or type(self.contain_memory) is not bool:
            raise ValueError('Invalid database_order or contain_memory')
        if self.validation_mode not in {'full', 'sampled'}:
            raise ValueError('validation_mode must be full or sampled')
        if type(self.validation_samples) is not int or self.validation_samples < 1:
            raise ValueError('validation_samples must be positive')
        if not self.databases or len(set(self.databases)) != len(self.databases) or not set(self.databases) <= {'nexoradb', 'postgresql', 'mongodb'}:
            raise ValueError('Invalid or duplicate databases')
        if not self.workloads or len(set(self.workloads)) != len(self.workloads) or not set(self.workloads) <= {'ycsb-a', 'ycsb-b', 'ycsb-c', 'indexed-read'}:
            raise ValueError('Only certified workloads A/B/C and indexed-read are supported')
        if not self.clients or any(type(n) is not int or not 1 <= n <= 500 for n in self.clients) or len(set(self.clients)) != len(self.clients):
            raise ValueError('clients must be unique integers in 1..500')
        if not self.working_sets or any(type(n) not in (float, int) or not 0 < n <= 1 for n in self.working_sets) or len(set(self.working_sets)) != len(self.working_sets):
            raise ValueError('working_sets must be unique fractions in (0,1]')
        if min(int(self.records * n) for n in self.working_sets) < max(self.clients):
            raise ValueError('Every working set must contain at least one key per client')
        if not self.index_policies or len(set(self.index_policies)) != len(self.index_policies) or not set(self.index_policies) <= {'before-load', 'after-load'}:
            raise ValueError('Invalid or duplicate index policies')
        if type(self.repetitions) is not int or self.repetitions < 1:
            raise ValueError('repetitions must be positive')
        for name in ('measurement_seconds', 'sample_interval_seconds', 'max_queue_delay_seconds'):
            value = getattr(self, name)
            if type(value) not in (float, int) or not math.isfinite(value) or value <= 0:
                raise ValueError(f'{name} must be finite and positive')
        if self.job_timeout_seconds is not None and (type(self.job_timeout_seconds) not in (float, int)
                or not math.isfinite(self.job_timeout_seconds) or self.job_timeout_seconds <= 0):
            raise ValueError('job_timeout_seconds must be null (unlimited) or finite and positive')
        if type(self.warmup_seconds) not in (float, int) or not math.isfinite(self.warmup_seconds) or self.warmup_seconds < 0:
            raise ValueError('warmup_seconds must be finite and nonnegative')
        if self.max_operations is not None and (type(self.max_operations) is not int or self.max_operations < max(self.clients)):
            raise ValueError('max_operations must be at least max(clients)')
        if self.load_mode not in {'closed-loop', 'open-loop'}:
            raise ValueError('Invalid load_mode')
        if self.load_mode == 'open-loop' and (type(self.target_rate) not in (float, int) or not math.isfinite(self.target_rate) or self.target_rate <= 0):
            raise ValueError('Open-loop requires a finite positive target_rate')
        if self.distribution not in {'uniform', 'zipfian'} or type(self.zipf_theta) not in (float, int) or not 0 < self.zipf_theta < 1:
            raise ValueError('distribution must be uniform/zipfian; zipf_theta must be in (0,1)')
        if self.cache_mode != 'warm-conditioned' or self.comparison_scope != 'local-deployments':
            raise ValueError('Only warm-conditioned local deployment comparisons are certified; cold-cache is NOT_SUPPORTED')

    @classmethod
    def read(cls, path):
        with open(path, encoding='utf-8') as source:
            return cls(**json.load(source))

    def digest(self):
        return hashlib.sha256(json.dumps(asdict(self), sort_keys=True).encode()).hexdigest()

    def jobs(self):
        jobs = []
        rng = random.Random(self.seed)
        for repetition in range(self.repetitions):
            engines = list(self.databases)
            if self.database_order == 'randomized':
                rng.shuffle(engines)
            for engine in engines:
                policies = list(self.index_policies)
                rng.shuffle(policies)
                for policy in policies:
                    cells = [(w, c, f) for w in self.workloads for c in self.clients for f in self.working_sets]
                    random.Random(self.seed + repetition).shuffle(cells)
                    jobs.append({'id': f'{len(jobs):04d}', 'database': engine,
                                 'repetition': repetition + 1, 'index_policy': policy,
                                 'cells': [{'workload': w, 'clients': c, 'working_set': f} for w, c, f in cells]})
        return jobs

    def plan(self):
        jobs = self.jobs()
        phases = sum(len(j['cells']) for j in jobs)
        seconds = phases * (self.warmup_seconds + self.measurement_seconds)
        return {'config': asdict(self), 'config_sha256': self.digest(), 'jobs': jobs,
                'job_count': len(jobs), 'measured_workload_trials': phases,
                'total_records_to_load': len(jobs) * self.records,
                'minimum_workload_hours_excluding_load_validate_restore': seconds / 3600,
                'primary_dataset': self.records == PRIMARY_RECORDS,
                'cache_claim': 'warm-conditioned; not cold and not an entirely RAM-resident dataset',
                'dry_plan_only': True}
