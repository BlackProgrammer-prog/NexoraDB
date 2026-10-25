import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from scale_lab.progress import Dashboard, WorkerProgress, bar, duration, snapshot
from scale_lab.protocol import atomic_json
from scale_lab.protocol_config import ProtocolConfig


class ProgressTests(unittest.TestCase):
    def test_native_load_dashboard_exposes_compaction_and_pipeline(self):
        with tempfile.TemporaryDirectory() as folder:
            display = Dashboard(folder,io.StringIO(),tty=True)
            lines = display.render({'engine_metrics': {'l0_files': 3,
                'pending_compaction_bytes': 1024**2, 'stall_micros': 2000000,
                'write_path': {'wal_syncs': 7, 'wal_sync_latency': {'p99_us': 10000}}},
                'producer_wait_seconds': 1.5, 'batch_ack_seconds': 4.0})
            self.assertTrue(any('L0=3' in line and 'stalls=2.00s' in line for line in lines))
            self.assertTrue(any('producer wait=1.5s' in line for line in lines))
            self.assertTrue(any('WAL sync cumulative p99: 10.0ms' in line for line in lines))
            display.close()
    def test_known_unknown_and_empty_progress(self):
        self.assertIn('25.0%', bar(25, 100))
        self.assertIn('100.0%', bar(101, 100))
        self.assertIn('--%', bar(0, None))
        self.assertIn('100.0%', bar(0, 0))
        self.assertEqual(duration(3661), '01:01:01')
        self.assertEqual(duration(None), '--')

    def test_worker_persists_real_counts_and_resets_on_new_stage(self):
        with tempfile.TemporaryDirectory() as folder:
            progress = WorkerProgress(folder, atomic_json, interval=0)
            progress.start('loading', 100)
            progress.update(25)
            value = json.loads((Path(folder) / 'progress.json').read_text())
            self.assertEqual(value['current'], 25)
            self.assertGreater(value['average_per_second'], 0)
            self.assertIsNotNone(value['eta_seconds'])
            progress.start('building-indexes')
            value = json.loads((Path(folder) / 'progress.json').read_text())
            self.assertEqual(value['current'], 0)
            self.assertIsNone(value['total'])
            self.assertIsNone(value['eta_seconds'])

    def test_tty_dashboard_draws_in_place_and_keeps_log(self):
        with tempfile.TemporaryDirectory() as folder:
            stream = io.StringIO()
            display = Dashboard(folder, stream, tty=True)
            value = {'job': '0001', 'stage': 'loading', 'database': 'postgresql',
                     'current': 25_000_000, 'total': 100_000_000, 'unit': 'records',
                     'average_per_second': 50000}
            display.update(value)
            display.update({**value, 'current': 26_000_000})
            display.close()
            self.assertIn('25.0%', stream.getvalue())
            self.assertIn('26.0%', stream.getvalue())
            self.assertIn('\x1b[9A', stream.getvalue())
            logged = [json.loads(line) for line in (Path(folder) / 'events.jsonl').read_text().splitlines()]
            self.assertEqual(logged[0]['database'], 'postgresql')

    def test_redirected_output_is_json_without_terminal_controls(self):
        with tempfile.TemporaryDirectory() as folder:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                display = Dashboard(folder, io.StringIO(), tty=False)
                display.update({'job': '0', 'stage': 'loading'})
                display.close()
            self.assertNotIn('\x1b', output.getvalue())
            for line in output.getvalue().splitlines():
                self.assertEqual(json.loads(line)['stage'], 'loading')

    def test_snapshot_ignores_previous_stage_counts(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            atomic_json(path / 'checkpoint.json', {'status': 'warming', 'cell': 0, 'workload': 'ycsb-c'})
            atomic_json(path / 'progress.json', {'stage': 'loading', 'current': 100, 'total': 100})
            job = {'id': '0000', 'database': 'mongodb', 'cells': [{}]}
            value = snapshot(path, job, 0, 3, 0, 36)
            self.assertNotIn('current', value)
            self.assertEqual(value['stage'], 'warming')
            self.assertEqual(value['current_trial'], '1/1')

    def test_compact_has_no_job_watchdog(self):
        path = Path(__file__).resolve().parents[1] / 'config' / 'compact-100m.json'
        self.assertIsNone(ProtocolConfig.read(path).job_timeout_seconds)
        with self.assertRaises(ValueError):
            ProtocolConfig(job_timeout_seconds=0)
