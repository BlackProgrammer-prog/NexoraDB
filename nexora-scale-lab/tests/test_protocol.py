import copy
import json
import os
import random
import tempfile
import time
import unittest
import subprocess
from dataclasses import asdict, replace
from pathlib import Path

from scale_lab.adapters import Adapter
from scale_lab.dataset import DatasetConfig, record
from scale_lab.integrity import ContentDigest, ExpectedWrites, full_validate, sampled_validate, restore
from scale_lab.protocol_config import ProtocolConfig
from scale_lab.reporting import interval, report
from scale_lab.resources import Sampler
from scale_lab.timed import operation, replay_writes, trial, zipf_rank


class MemoryAdapter(Adapter):
    engine_name = 'test-only'
    def __init__(self, dataset):
        super().__init__()
        self.values = {record(dataset, i)['id']: record(dataset, i) for i in range(dataset.records)}
    def count(self):
        return len(self.values)
    def iter_records(self, batch_size=1000):
        yield from self.values.values()
    def read_by_id(self, key):
        return copy.deepcopy(self.values.get(key))
    def update(self, key, status):
        self.values[key]['status'] = status


def tiny_config(**overrides):
    values = dict(records=23, clients=[2], repetitions=1, warmup_seconds=.005,
                  measurement_seconds=.03, max_operations=16, sample_interval_seconds=.02,
                  index_policies=['before-load', 'after-load'])
    values.update(overrides)
    return ProtocolConfig(**values)


class ProtocolUnitTests(unittest.TestCase):
    def test_explicit_matched_cache_profile_is_opt_in(self):
        from scale_lab.protocol import adapter_for
        config = ProtocolConfig.read(Path(__file__).resolve().parents[1] / 'config/compact-matched-cache-100m.json')
        self.assertEqual(config.native_block_cache_mb,4096)
        self.assertEqual(config.native_memory_budget_mb,5120)
        self.assertEqual(config.records,100000000)
        adapter = adapter_for('nexoradb','scale_lab_test','/not-opened',config)
        self.assertEqual(adapter.block_cache_bytes,4096*1024**2)
        self.assertEqual(ProtocolConfig().native_memory_budget_mb,256)
        with self.assertRaises(ValueError):
            tiny_config(native_memory_budget_mb=256,native_block_cache_mb=256)

    def test_compact_keeps_100m_and_has_42_minute_workload_budget(self):
        path = Path(__file__).resolve().parents[1] / 'config' / 'compact-100m.json'
        config = ProtocolConfig.read(path)
        plan = config.plan()
        self.assertEqual(config.records, 100_000_000)
        self.assertEqual(plan['job_count'], 3)
        self.assertEqual(plan['measured_workload_trials'], 36)
        self.assertEqual(plan['total_records_to_load'], 300_000_000)
        self.assertAlmostEqual(plan['minimum_workload_hours_excluding_load_validate_restore'], .7)
        self.assertEqual(config.validation_mode, 'sampled')
        self.assertEqual([job['database'] for job in config.jobs()],
                         ['mongodb', 'nexoradb', 'postgresql'])
        self.assertTrue(config.contain_memory)

    def test_sampled_validation_is_explicit_and_detects_sample_corruption(self):
        dataset = DatasetConfig(23)
        adapter = MemoryAdapter(dataset)
        baseline = full_validate(adapter, dataset)['baseline']
        result = sampled_validate(adapter, dataset, baseline, samples=3)
        self.assertEqual(result['record_count'], 23)
        self.assertLess(result['verified_records'], 23)
        self.assertFalse(result['stored_dataset_digest_verified'])
        adapter.values['000000000000']['status'] = 'corrupt'
        with self.assertRaises(RuntimeError):
            sampled_validate(adapter, dataset, baseline, samples=3)

    def test_schedule_balanced_and_deterministic(self):
        config = ProtocolConfig()
        plan = config.plan()
        self.assertEqual(plan['job_count'], 30)
        self.assertEqual(plan['measured_workload_trials'], 720)
        self.assertEqual(plan['total_records_to_load'], 3_000_000_000)
        self.assertEqual(config.jobs(), ProtocolConfig().jobs())
        self.assertNotEqual(config.jobs(), replace(config, seed=2027).jobs())

    def test_strict_configuration(self):
        for invalid in ({'clients': [0]}, {'repetitions': 0}, {'cache_mode': 'cold'},
                        {'measurement_seconds': float('nan')}, {'load_mode': 'open-loop'},
                        {'workloads': ['ycsb-f']}, {'working_sets': [0]}, {'zipf_theta': 1}):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                tiny_config(**invalid)

    def test_zipf_sampler_distribution(self):
        rng = random.Random(123)
        counts = [0] * 8
        for _ in range(20000):
            counts[zipf_rank(rng, 8, .7)] += 1
        normalizer = sum(k ** -.7 for k in range(1, 9))
        for index, count in enumerate(counts):
            self.assertLess(abs(count / 20000 - (index + 1) ** -.7 / normalizer), .015)

    def test_disjoint_access_ownership(self):
        config = tiny_config(distribution='zipfian')
        dataset = DatasetConfig(23)
        for worker in range(3):
            rng = random.Random(worker)
            for sequence in range(50):
                key, _ = operation(config, dataset, 'ycsb-a', 3, 1., 42, worker, sequence, rng)
                self.assertTrue(0 <= key < 23)
                self.assertEqual(key % 3, worker)

    def test_write_replay_full_validation_and_restore(self):
        config = tiny_config()
        dataset = DatasetConfig(23)
        adapter = MemoryAdapter(dataset)
        baseline = full_validate(adapter, dataset)['baseline']
        result = trial(adapter, dataset, config, 'ycsb-a', 2, 1., 42, .2, 80)
        self.assertEqual(result['counts'].get('failed', 0), 0)
        self.assertGreater(result['counts']['updates'], 0)
        with tempfile.TemporaryDirectory() as folder:
            writes = ExpectedWrites(Path(folder) / 'writes.sqlite')
            try:
                replay_writes(writes, dataset, config, result)
                full_validate(adapter, dataset, baseline, writes)
                restore(adapter, dataset, writes)
                full_validate(adapter, dataset, baseline)
            finally:
                writes.close()

    def test_integrity_detects_corruption_not_just_counts(self):
        dataset = DatasetConfig(23)
        adapter = MemoryAdapter(dataset)
        baseline = full_validate(adapter, dataset)['baseline']
        adapter.values['000000000001']['balance_cents'] += 1
        with self.assertRaises(RuntimeError):
            full_validate(adapter, dataset, baseline)

    def test_unknown_write_outcomes_refuse_replay(self):
        with self.assertRaises(RuntimeError):
            replay_writes(None, None, None, {'counts': {'failed': 1}})

    def test_open_loop_rejected_slots_are_counted(self):
        config = tiny_config(load_mode='open-loop', target_rate=1000, max_queue_delay_seconds=.001)
        dataset = DatasetConfig(23)
        class SlowAdapter(MemoryAdapter):
            def read_by_id(self, key):
                time.sleep(.006)
                return super().read_by_id(key)
        result = trial(SlowAdapter(dataset), dataset, config, 'ycsb-c', 1, 1., 42, .03)
        self.assertEqual(result['counts']['attempted'], 30)
        self.assertGreater(result['counts']['rejected_load_slots'], 0)
        self.assertEqual(result['histograms']['response_latency']['count'], 30)
        self.assertLess(result['histograms']['driver_latency']['count'], 30)

    def test_student_t_interval_and_no_single_run_ci(self):
        result = interval([1, 2, 3, 4, 5])
        self.assertEqual(result['mean'], 3)
        self.assertAlmostEqual(result['ci95_high'], 4.9629, places=3)
        self.assertIsNone(interval([1])['ci95_high'])

    def test_resource_sampler_has_raw_samples(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'samples.jsonl'
            with Sampler(path, [os.getpid()], interval=.01, combined=True) as sampler:
                time.sleep(.025)
            self.assertGreaterEqual(sampler.summary()['sample_count'], 2)
            for line in path.read_text().splitlines():
                self.assertIn('groups', json.loads(line))
            maximum = max(json.loads(line)['deployment_memory']['pss_bytes']
                          for line in path.read_text().splitlines())
            self.assertEqual(sampler.summary()['deployment_sampled_peaks']['pss_bytes'], maximum)

    def test_failed_attempt_is_retained_in_report_audit(self):
        from scale_lab.protocol import atomic_json
        config = tiny_config()
        class FailingAdapter(MemoryAdapter):
            def read_by_id(self, key):
                raise TimeoutError('controlled test fault')
        raw = trial(FailingAdapter(DatasetConfig(23)), DatasetConfig(23), config,
                    'ycsb-c', 2, 1., 42, .05, 16)
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            atomic_json(directory / 'manifest.json', {'config': asdict(config), 'status': 'failed',
                'schedule': [{'id': '0000', 'database': 'nexoradb', 'cells': [{}]}],
                'comparison_scope': 'local-deployments', 'claims': [], 'unsupported': []})
            attempt = directory / 'job-0000' / 'attempt-0000'
            atomic_json(attempt / 'checkpoint.json', {'status': 'failed'})
            atomic_json(attempt / 'cell-0000' / 'measurement.json', {**raw, 'status': 'measured-not-yet-validated'})
            report(directory)
            summary = json.loads((directory / 'summary.json').read_text())
            self.assertFalse(summary['protocol_passed'])
            self.assertEqual(summary['failed_or_incomplete_attempts'][0]['counts']['failed'], 16)
            self.assertEqual(summary['completed_trials'], 0)

    @unittest.skipUnless(Path('/mnt/c/Program Files/nodejs/node.exe').is_file(), 'Node syntax checker not installed')
    def test_report_javascript_syntax(self):
        template = Path(__file__).resolve().parents[1] / 'scale_lab' / 'report.html'
        script = template.read_text().rsplit('<script>', 1)[1].split('</script>', 1)[0]
        result = subprocess.run(['/mnt/c/Program Files/nodejs/node.exe', '--check'],
                                input=script, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(os.environ.get('SCALE_LAB_TEST_PROTOCOL') == '1', 'short real protocol integration opt-in')
class RealProtocolTests(unittest.TestCase):
    def test_exclusive_bson_process_capacity_pipeline_integrity(self):
        from scale_lab.protocol import run_job
        config=tiny_config(databases=['nexoradb'],index_policies=['before-load'],
                           native_ingestion_mode='exclusive-bulk',native_bson_processes=2,
                           native_wal_range_sync_mb=0)
        with tempfile.TemporaryDirectory(prefix='scale-lab-encoded-test-') as folder:
            destination=Path(folder)
            run_job(config,config.jobs()[0],destination)
            state=json.loads((destination/'checkpoint.json').read_text())
            self.assertEqual(state['status'],'completed')
            metadata=json.loads((destination/'engine.json').read_text())
            self.assertEqual(metadata['load_pipeline']['producer_processes'],2)
            self.assertIn('insert_many_bson_ingest',metadata['ingestion_api'])
            ingestion=json.loads((destination/'ingestion.json').read_text())
            self.assertEqual(ingestion['records'],config.records)
            self.assertTrue(state['private_store_removed'])

    def test_native_private_store_cleanup_never_logically_drops_collection(self):
        from unittest.mock import patch
        from scale_lab.protocol import run_job
        from scale_lab.adapters import NexoraAdapter
        config = tiny_config(databases=['nexoradb'], index_policies=['before-load'])
        with tempfile.TemporaryDirectory(prefix='scale-lab-cleanup-test-') as folder:
            destination = Path(folder)
            with patch.object(NexoraAdapter, 'cleanup', side_effect=AssertionError('logical drop not allowed')):
                run_job(config, config.jobs()[0], destination)
            checkpoint = json.loads((destination / 'checkpoint.json').read_text())
            self.assertEqual(checkpoint['status'], 'completed')
            self.assertTrue(checkpoint['private_store_removed'])
            self.assertFalse(Path(checkpoint['private_store']).exists())
            diagnostics = [json.loads(line) for line in (destination/'load-engine.jsonl').read_text().splitlines()]
            self.assertEqual(diagnostics[-1]['records'],config.records)
            self.assertGreater(diagnostics[-1]['batch_ack_seconds'],0)
            self.assertGreaterEqual(diagnostics[-1]['producer_wait_seconds'],0)
            self.assertIn('pending_compaction_bytes',diagnostics[-1]['engine_metrics'])
            self.assertIn('stall_micros',diagnostics[-1]['engine_metrics'])
            stages = [json.loads(line) for line in (destination/'stage-memory.jsonl').read_text().splitlines()]
            self.assertTrue(stages)
            self.assertTrue(all(not sample['allocator_maintenance']['attempted'] for sample in stages))

    def test_shutdown_error_preserves_original_load_failure(self):
        from unittest.mock import patch
        from scale_lab.protocol import run_job
        from scale_lab.adapters import NexoraAdapter
        config = tiny_config(databases=['nexoradb'], index_policies=['before-load'])
        with tempfile.TemporaryDirectory(prefix='scale-lab-failure-test-') as folder:
            destination = Path(folder)
            with patch.object(NexoraAdapter, 'batch_insert', side_effect=RuntimeError('controlled load failure')), \
                 patch.object(NexoraAdapter, 'disconnect', side_effect=RuntimeError('controlled shutdown failure')):
                with self.assertRaisesRegex(RuntimeError, 'controlled load failure'):
                    run_job(config, config.jobs()[0], destination)
            checkpoint = json.loads((destination / 'checkpoint.json').read_text())
            self.assertEqual(checkpoint['status'], 'failed')
            self.assertEqual(checkpoint['failed_stage'], 'loading')
            self.assertEqual(checkpoint['error_message'], 'controlled load failure')
            self.assertEqual(checkpoint['cleanup_error_message'], 'controlled shutdown failure')

    def test_short_end_to_end_all_engines_reports_and_resume(self):
        from scale_lab.protocol import execute
        # Six tiny jobs; never the large user experiment. Real engines, full
        # every-field validation, A/B/C and username lookup, both index policies.
        config = tiny_config(job_timeout_seconds=None, contain_memory=os.geteuid() == 0,
                             load_prefetch_batches=2)
        with tempfile.TemporaryDirectory(prefix='scale-lab-protocol-test-') as folder:
            destination = Path(folder) / 'results'
            result = execute(config, destination)
            self.assertEqual(result['status'], 'completed')
            summary = json.loads((destination / 'summary.json').read_text())
            self.assertEqual(summary['completed_trials'], 24)
            self.assertFalse(summary['protocol_passed'])  # Small tests cannot masquerade as 100M.
            for name in ('summary.json', 'summary.csv', 'ingestion.csv', 'summary.md', 'report.html'):
                self.assertTrue((destination / name).is_file())
            execute(config, destination, resume=True)  # No completed job is rerun.
            from scale_lab.continuation import prepare_continuation
            ordered = replace(config, databases=['mongodb', 'nexoradb', 'postgresql'],
                              database_order='listed')
            fork = Path(folder) / 'continued'
            prepared = prepare_continuation(ordered, destination, fork)
            self.assertEqual(prepared['status'], 'prepared-not-executed')
            manifest = json.loads((fork / 'manifest.json').read_text())
            self.assertEqual(len(manifest['imported_jobs']), 2)
            for imported in manifest['imported_jobs']:
                original = Path(imported['original_attempt']) / 'ingestion.json'
                copied = fork / ('job-' + imported['job']) / 'attempt-0000' / 'ingestion.json'
                self.assertEqual(original.read_bytes(), copied.read_bytes())
            execute(ordered, fork, resume=True)
            preserved = Path(folder) / 'preserved-all'
            result = prepare_continuation(ordered, fork, preserved, preserve_completed=True)
            self.assertEqual(result['next_databases'], [])
            self.assertEqual(result['preserved'], ['mongodb', 'nexoradb', 'postgresql'])
            imports = json.loads((preserved / 'manifest.json').read_text())['imported_jobs']
            self.assertEqual(len(imports), 6)
            from scale_lab.protocol import binary_digest
            for item in imports:
                root = preserved / ('job-' + item['job']) / 'attempt-0000'
                for name, digest in item['artifact_sha256'].items():
                    self.assertEqual(binary_digest(root / name), digest)
            execute(ordered, preserved, resume=True)
            self.assertFalse(json.loads((fork / 'summary.json').read_text())['protocol_passed'])
            with self.assertRaises(ValueError):
                prepare_continuation(replace(ordered, records=24), destination, Path(folder) / 'invalid')
            with self.assertRaises(ValueError):
                execute(replace(config, seed=99), destination, resume=True)
