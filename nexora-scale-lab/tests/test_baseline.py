import json
import tempfile
import unittest
from pathlib import Path

from scale_lab.baseline import audit
from scale_lab.protocol_config import ProtocolConfig


class BaselineTests(unittest.TestCase):
    def test_profile_is_matched_repeated_and_not_capacity(self):
        config = ProtocolConfig.read(Path(__file__).resolve().parents[1] / 'config/baseline-short.json')
        self.assertEqual(config.repetitions, 5)
        self.assertEqual(config.batch_size, 1000)
        self.assertEqual(config.native_ingestion_mode, 'transactional')
        self.assertEqual(config.native_bson_processes, 0)
        self.assertEqual(config.validation_mode, 'full')
        self.assertEqual(len(config.jobs()), 10)

    def test_audit_is_read_only_and_excludes_incomplete_jobs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'manifest.json').write_text(json.dumps(dict(status='completed', source_sha256='s', config_sha256='c', config={})))
            for number, (status, rate) in enumerate([('completed', 10), ('completed', 20), ('failed', 1000)]):
                attempt = root / f'job-{number}/attempt-0000'
                cell = attempt / 'cell-0000'
                cell.mkdir(parents=True)
                (attempt / 'checkpoint.json').write_text(json.dumps(dict(status=status, job=dict(database='nexoradb', index_policy='before-load'))))
                (cell / 'measurement.json').write_text(json.dumps(dict(workload='ycsb-c', clients=1, working_set=1.0, throughput=dict(successful_per_second=rate))))
            before = {str(p): p.read_bytes() for p in root.rglob('*') if p.is_file()}
            result = audit(root)
            self.assertEqual(result['groups'][0]['n'], 2)
            self.assertEqual(result['groups'][0]['mean_ops'], 15)
            self.assertGreater(result['groups'][0]['cv_percent'], 0)
            self.assertEqual(before, {str(p): p.read_bytes() for p in root.rglob('*') if p.is_file()})
