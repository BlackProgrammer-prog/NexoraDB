import copy
import json
import os
from pathlib import Path
import sys
import subprocess
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import research_ab as ab
from research_workload import edges, reachable


def config():
    value = json.loads((ROOT / 'nexora-scale-lab/config/research-smoke-v1.json').read_text())
    value['require_loaded_artifacts'] = False  # Synthetic fixtures do not load the native engine.
    return value


class ResearchContractTests(unittest.TestCase):
    def test_plan_is_deterministic_and_bracketed(self):
        cfg = config()
        self.assertEqual(ab.plan(cfg), ab.plan(cfg))
        schedule = ab.plan(cfg)['order']
        self.assertEqual(len(schedule), 9)
        for i in range(0, len(schedule), 3):
            self.assertEqual(schedule[i]['arm'], schedule[i + 2]['arm'])
            self.assertNotEqual(schedule[i]['arm'], schedule[i + 1]['arm'])

    def test_invalid_config_and_nonfinite_metrics(self):
        for key, value in [('repetitions', 1), ('timeout_seconds', 601), ('max_cv', float('nan'))]:
            cfg = config()
            cfg[key] = value
            with self.assertRaises(ValueError):
                ab.plan(cfg)
        for field, value in [('correct', False), ('errors', 1), ('ops_s', float('nan')),
                             ('p99_us', -1), ('operations', 0), ('workload_sha256', 'wrong')]:
            sample = self.sample()
            sample[field] = value
            with self.assertRaises(ValueError):
                ab.validate_sample(sample, ab.plan(config())['workload_sha256'])

    def sample(self):
        return {'correct': True, 'errors': 0, 'operations': 30, 'ops_s': 1000,
                'p99_us': 100, 'resource_scope': 'combined',
                'workload_sha256': ab.plan(config())['workload_sha256']}

    def fixture(self, directory, drift=False):
        cfg = config()
        schedule = ab.plan(cfg)
        ab.exclusive_json(directory / 'manifest.json', {'config': cfg, 'plan': schedule})
        for i, entry in enumerate(schedule['order']):
            sample = self.sample()
            if entry['arm'] == 'B':
                sample.update(ops_s=1200, p99_us=80)
            if drift and i == 2:
                sample['ops_s'] *= 2
            ab.exclusive_json(directory / f'{i:03d}.json', {**entry, 'sample': sample})
        ab.exclusive_json(directory / 'completed.json', {'samples': len(schedule['order']),
            'file_sha256': {p.name: ab.digest(p) for p in directory.iterdir()}})

    def test_analysis_ratios_and_no_automatic_winner(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.fixture(directory)
            result = ab.analyze(directory)
            self.assertEqual(result['classification'], 'descriptive-stable')
            self.assertAlmostEqual(result['B_over_A_ops_median'], 1.2)
            self.assertAlmostEqual(result['B_over_A_p99_median'], .8)
            self.assertFalse(result['publication_eligible'])
            with self.assertRaises(FileExistsError):
                ab.exclusive_json(directory / 'manifest.json', {})

    def test_drift_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.fixture(directory, drift=True)
            self.assertEqual(ab.analyze(directory)['classification'], 'inconclusive-environment-drift')

    def test_incomplete_or_modified_evidence_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.fixture(directory)
            with (directory / '000.json').open('a') as stream:
                stream.write(' ')
            with self.assertRaises(ValueError):
                ab.analyze(directory)
            (directory / 'completed.json').unlink()
            with self.assertRaises(ValueError):
                ab.analyze(directory)

    def test_graph_oracle_independent_bfs_and_no_duplicates(self):
        for count in (32, 64, 100):
            for source in range(count):
                one = edges(source, count) - {source}
                two = {target for node in one for target in edges(node, count)} - one - {source}
                self.assertEqual(reachable(source, count), {**dict.fromkeys(one, 1), **dict.fromkeys(two, 2)})

    def test_real_subprocess_roundtrip_and_refuses_existing_directory(self):
        cfg = config()
        code = ("import json,os,hashlib; w=json.loads(os.environ['NEXORA_RESEARCH_WORKLOAD']); "
                "print(json.dumps(dict(correct=True,errors=0,operations=30,ops_s=1000,p99_us=100,"
                "resource_scope='synthetic unit-test process',workload_sha256=hashlib.sha256("
                "json.dumps(w,sort_keys=True,allow_nan=False).encode()).hexdigest())))")
        cfg['commands'] = {arm: [sys.executable, '-c', code] for arm in ('A', 'B')}
        cfg['binaries'] = {arm: [sys.executable] for arm in ('A', 'B')}
        with tempfile.TemporaryDirectory() as name, patch.object(ab, 'source_identity', return_value={'source_sha256': 'test-source'}):
            target = Path(name) / 'fresh'
            result = ab.run(cfg, target)
            self.assertEqual(result['classification'], 'descriptive-stable')
            self.assertEqual(len(list(target.glob('*.stdout'))), 9)
            with self.assertRaises(FileExistsError):
                ab.run(cfg, target)

    def test_timeout_preserves_incomplete_evidence(self):
        cfg = config()
        cfg['timeout_seconds'] = .05
        cfg['commands'] = {arm: [sys.executable, '-c', 'import time; time.sleep(10)'] for arm in ('A', 'B')}
        cfg['binaries'] = {arm: [sys.executable] for arm in ('A', 'B')}
        with tempfile.TemporaryDirectory() as name:
            target = Path(name) / 'fresh'
            with self.assertRaises(subprocess.TimeoutExpired):
                ab.run(cfg, target)
            self.assertTrue((target / '000.stderr').exists())
            self.assertFalse((target / 'completed.json').exists())
            with self.assertRaises(ValueError):
                ab.analyze(target)

    @unittest.skipUnless(os.environ.get('NEXORA_RESEARCH_NATIVE') == '1', 'Opt-in small native integration')
    def test_native_three_scenarios(self):
        interpreter = ROOT / 'nexora-scale-lab/.venv/bin/python'
        for scenario in ('document-oltp', 'document-graph', 'low-memory'):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(interpreter), str(ROOT / 'scripts/research_workload.py'),
                                         '--scenario', scenario], capture_output=True, text=True, timeout=60, check=True)
                row = json.loads(result.stdout)
                self.assertTrue(row['correct'])
                self.assertEqual(row['errors'], 0)
                self.assertFalse(row['publication_eligible'])

    @unittest.skipUnless(os.environ.get('NEXORA_RESEARCH_NATIVE') == '1', 'Opt-in alternate native loading')
    def test_native_selection_loads_the_actual_declared_copy(self):
        source = ROOT / 'NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so'
        with tempfile.TemporaryDirectory() as name:
            target = Path(name) / source.name
            shutil.copyfile(source, target)
            env = os.environ.copy()
            env['NEXORA_RESEARCH_NATIVE_PATH'] = str(target)
            result = subprocess.run([str(ROOT / 'nexora-scale-lab/.venv/bin/python'),
                                     str(ROOT / 'scripts/research_workload.py'), '--scenario', 'document-graph'],
                                    capture_output=True, text=True, timeout=60, check=True, env=env)
            self.assertEqual(json.loads(result.stdout)['loaded_artifacts'], {str(target): ab.digest(target)})


if __name__ == '__main__':
    unittest.main()
