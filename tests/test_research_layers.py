import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import research_layers as layers
from research_ab import bootstrap_median_interval, verify_loaded, archive_sources, digest
from research_resources import ProcessSampler


class LayerTests(unittest.TestCase):
    def test_fixture_deterministic_and_bounded(self):
        self.assertEqual(layers.make_fixture(64, 100), layers.make_fixture(64, 100))
        for records, operations in [(1, 10), (10001, 10), (64, 100001)]:
            with self.assertRaises(ValueError):
                layers.make_fixture(records, operations)

    def test_oracle_rejects_wrong_document_and_empty_response(self):
        fixture = layers.make_fixture(64, 10)
        expected = {doc['_id']: doc for doc in fixture['documents']}
        report = layers.measure(fixture, lambda key: json.dumps([expected[key]]), json.loads)
        self.assertTrue(report['correct'])
        for wrong in ('[]', '[{"_id":"wrong"}]'):
            with self.assertRaises(ValueError):
                layers.measure(fixture, lambda key: wrong, json.loads)

    def test_http_200_statement_errors_and_nested_semantics(self):
        body = {'statementErrors': [], 'raw': {'statements': [{'success': True, 'documents': [{'nested': {'a': None}}]}]}}
        self.assertEqual(layers.decode_http_documents(json.dumps(body)), [{'nested': {'a': None}}])
        broken = copy.deepcopy(body)
        broken['raw']['statements'][0]['success'] = False
        with self.assertRaises(ValueError):
            layers.decode_http_documents(json.dumps(broken))
        broken = copy.deepcopy(body)
        broken['statementErrors'] = ['failure']
        with self.assertRaises(ValueError):
            layers.decode_http_documents(json.dumps(broken))

    def test_bootstrap_is_seeded_and_uses_blocks(self):
        self.assertEqual(bootstrap_median_interval([1.2] * 5, 2026), [1.2, 1.2])
        values = [1, 1.1, .9, 1.2, 1.3]
        self.assertEqual(bootstrap_median_interval(values, 2026), bootstrap_median_interval(values, 2026))

    def test_loaded_artifact_mismatch_rejected(self):
        verify_loaded({'loaded_artifacts': {'native.so': 'abc'}}, {'native.so': 'abc'})
        with self.assertRaises(ValueError):
            verify_loaded({'loaded_artifacts': {'native.so': 'old'}}, {'native.so': 'new'})

    def test_same_pid_cannot_be_counted_twice(self):
        with self.assertRaises(ValueError):
            ProcessSampler({'engine': [os.getpid()], 'generator': [os.getpid()]})

    def test_source_archive_rejects_changed_file(self):
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaises(ValueError):
                archive_sources({'source_files': {'scripts/research_ab.py': 'wrong'}}, Path(name) / 'source.tar.gz')

    def test_pid_reuse_and_overlapping_trees_rejected(self):
        with patch('research_resources.process', return_value={'start_ticks': 1}):
            sampler = ProcessSampler({'one': [10]})
        with patch('research_resources.process', return_value={'start_ticks': 2}):
            sampler.sample()
        self.assertEqual(sampler.peaks['one']['rss_peak_sampled_bytes'], 0)
        with patch('research_resources.process', return_value={'start_ticks': 1, 'cpu_seconds': 0, 'rss_bytes': 0, 'pss_bytes': 0}), \
             patch('research_resources.tree', side_effect=lambda roots: [10, 20] if roots == [10] else [20]):
            sampler = ProcessSampler({'engine': [10], 'generator': [20]})
            with self.assertRaises(ValueError):
                sampler.sample()

    def test_preregistration_is_immutable_and_requires_verified_baseline(self):
        import research_preregister as prereg
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            baseline = directory / 'baseline'
            baseline.mkdir()
            manifest = {'config': {'require_loaded_artifacts': True}, 'binaries': {'A': {'x.so': 'abc'}},
                        'binary_archive': {'A': {'x.so': 'binaries/abc/x.so'}}, 'source_archive_sha256': 'abc'}
            (baseline / 'manifest.json').write_text(json.dumps(manifest))
            (baseline / 'completed.json').write_text('{}')
            with patch.object(prereg, 'analyze', return_value={'classification': 'descriptive-stable'}):
                value = prereg.freeze(baseline, directory / 'new')
                self.assertEqual(value['hypotheses'], ['H1', 'H2', 'H3', 'H4', 'H5', 'H6'])
                self.assertFalse(value['publication_eligible'])
                with self.assertRaises(FileExistsError):
                    prereg.freeze(baseline, directory / 'new')

    def test_product_matrix_explicit_targets_not_installed_claims(self):
        value = json.loads((ROOT / 'nexora-scale-lab/config/research-products-v1.json').read_text())
        self.assertEqual(set(value['products']), {'mongodb', 'postgresql', 'sqlserver', 'oracle', 'neo4j'})
        self.assertFalse(value['publication_eligible'])
        self.assertTrue(all(row['version'] and row['edition'] for row in value['products'].values()))
        self.assertIsNone(value['products']['sqlserver']['installed_binary_observed'])
        self.assertEqual(value['products']['neo4j']['W1_document_model'], 'N/A')

    @unittest.skipUnless(os.environ.get('NEXORA_RESEARCH_LAYERS') == '1', 'Opt-in compiled/HTTP integration')
    def test_three_real_layers(self):
        with tempfile.TemporaryDirectory() as name:
            result = layers.run(Path(name) / 'fresh', records=64, operations=100)
            self.assertEqual(set(result['reports']), {'cpp', 'binding', 'http'})
            self.assertTrue(all(row['correct'] for row in result['reports'].values()))
            self.assertEqual(result['reports']['http']['authorization_checks'], {'missing_token': 401, 'denied_collection': 403})
            self.assertFalse(result['subtraction_is_component_cost'])
            self.assertEqual(set(result['reports']['http']['resources']['scopes']), {'engine_server', 'network_generator'})
            with self.assertRaises(FileExistsError):
                layers.run(Path(name) / 'fresh')

    @unittest.skipUnless(os.environ.get('NEXORA_RESEARCH_CGROUP') == '1', 'Opt-in root-only isolated cgroup')
    def test_actual_scoped_memory(self):
        from research_cgroup import run
        command = [sys.executable, str(ROOT / 'scripts/research_workload.py'), '--scenario', 'low-memory',
                   '--clients', '10', '--records', '1000', '--operations', '200']
        result = run(command)
        self.assertTrue(result['memory_limit_enforced'])
        self.assertEqual(result['cgroup']['after']['max_bytes'], 2 * 1024**3)
        self.assertTrue(all(value == 0 for value in result['cgroup']['event_deltas'].values()))


if __name__ == '__main__':
    unittest.main()
