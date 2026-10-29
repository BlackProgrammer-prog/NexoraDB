import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import research_acceptance as acceptance
from research_telemetry import snapshot, Telemetry


class AcceptanceTests(unittest.TestCase):
    def fixture(self):
        plan = acceptance.make_plan('long')
        row = {'classification': 'descriptive-stable', 'inference_minimum_blocks_met': True, 'driver_runtime_recorded': True,
               'B_over_A_ops_ci95': [.98, 1.02], 'B_over_A_p99_ci95': [.95, 1.05]}
        aa = {'1': copy.deepcopy(row), '10': copy.deepcopy(row)}
        layers = [{'reports': {name: {'ops_s': 1000, 'p99_us': 100} for name in ('cpp', 'binding', 'http')}} for _ in range(5)]
        return aa, layers, plan

    def test_long_is_explicit_and_plan_is_bounded(self):
        long = acceptance.make_plan('long')
        self.assertEqual(long['aa_configs'][0]['workload']['operations'], 1_000_000)
        self.assertTrue(long['aa_configs'][0]['workload']['allow_long'])
        self.assertEqual(long['aa_configs'][0]['workload']['warmup_operations'], 100000)
        short = acceptance.make_plan('short')
        self.assertFalse(short['aa_configs'][0]['workload']['allow_long'])
        self.assertLess(short['maximum_scheduled_seconds'], long['maximum_scheduled_seconds'])

    def test_accepts_controls_not_superiority(self):
        result = acceptance.acceptance(*self.fixture())
        self.assertTrue(all(result['parent_acceptance'].values()))
        self.assertFalse(result['publication_eligible'])
        self.assertEqual(result['hypotheses_accepted'], [])

    def test_rejects_drift_false_positive_and_unstable_layers(self):
        for failure in ('drift', 'null', 'layer', 'short'):
            aa, layers, plan = self.fixture()
            if failure == 'drift':
                aa['1']['classification'] = 'inconclusive-environment-drift'
            elif failure == 'null':
                aa['1']['B_over_A_ops_ci95'] = [1.01, 1.02]
            elif failure == 'layer':
                layers[0]['reports']['http']['p99_us'] = 1000
            else:
                plan['profile'] = 'short'
            self.assertFalse(all(acceptance.acceptance(aa, layers, plan)['parent_acceptance'].values()))

    def test_threshold_relaxation_is_rejected(self):
        aa, layers, plan = self.fixture()
        plan['thresholds']['max_bracket_drift'] = .50
        with self.assertRaises(ValueError):
            acceptance.acceptance(aa, layers, plan)

    def test_telemetry_records_missing_governor_without_claiming_measurement(self):
        value = snapshot([sorted(os.sched_getaffinity(0))[0]])
        self.assertIn('io', value['pressure'])
        self.assertIn('available', next(iter(value['cpu_policy'].values()))['scaling_governor'])
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / 'telemetry.jsonl'
            with Telemetry(path, [sorted(os.sched_getaffinity(0))[0]]):
                pass
            self.assertGreaterEqual(len(path.read_text().splitlines()), 2)

    def test_seal_missing_plan_or_modified_evidence_is_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name)
            (path / 'completed.json').write_text('{"sha256": {}}')
            with self.assertRaises(ValueError):
                acceptance.verify_seal(path)


if __name__ == '__main__':
    unittest.main()
