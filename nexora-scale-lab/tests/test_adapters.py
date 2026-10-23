import os
import tempfile
import unittest
import uuid
from unittest.mock import patch

from scale_lab.adapters import NexoraAdapter, PostgreSQLAdapter, MongoDBAdapter, NotSupported
from scale_lab.dataset import DatasetConfig, record
from scale_lab.runner import Latencies, load, run, validate


class HistogramTests(unittest.TestCase):
    def test_failed_requests_remain_in_statistics(self):
        class FailingAdapter:
            engine_name = 'test-only'
            def read_by_id(self, key):
                raise TimeoutError('controlled timeout')
            def count(self):
                return 10
        with patch('scale_lab.runner.validate', return_value={}):
            result = run(FailingAdapter(), DatasetConfig(10), clients=3, operations=29)
        self.assertEqual(result['counts']['attempted'], 29)
        self.assertEqual(result['counts']['failed'], 29)
        self.assertEqual(result['counts']['timeouts'], 29)
        self.assertGreater(result['latency']['maximum_ms'], 0)

    def test_quantiles_and_maximum(self):
        histogram = Latencies()
        for i in range(1, 1001):
            histogram.add(i * 1_000_000)
        summary = histogram.summary()
        self.assertEqual(summary['maximum_ms'], 1000)
        self.assertEqual(summary['average_ms'], 500.5)
        self.assertTrue(500 <= summary['p50_ms'] <= 505)
        self.assertTrue(990 <= summary['p99_ms'] <= 1000)


class AdapterConformance(unittest.TestCase):
    def exercise(self, adapter):
        config = DatasetConfig(31, batch_size=7)
        adapter.connect()
        prepared = False
        try:
            adapter.prepare_schema()
            prepared = True
            adapter.create_indexes()
            self.assertEqual(load(adapter, config)['records'], 31)
            self.assertEqual(adapter.read_by_id('000000000005'), record(config, 5))
            with self.assertRaises(RuntimeError):
                load(adapter, config)
            result = run(adapter, config, clients=3, operations=101)
            self.assertEqual(result['counts']['successful'], 101)
            self.assertEqual(result['counts']['attempted'], 101)
            self.assertEqual(result['counts'].get('failed', 0), 0)
            adapter.update('000000000005', 'changed')
            self.assertEqual(adapter.read_by_id('000000000005')['status'], 'changed')
            with self.assertRaises(RuntimeError):
                validate(adapter, config)
            adapter.update('000000000005', record(config, 5)['status'])
            adapter.restore_batch([('000000000005', 'restored'), ('000000000006', 'restored')])
            self.assertEqual(adapter.read_by_id('000000000006')['status'], 'restored')
            adapter.restore_batch([(f'{i:012d}', record(config, i)['status']) for i in (5, 6)])
            mixed = run(adapter, config, workload='ycsb-a', clients=3, operations=101)
            self.assertGreater(mixed['counts']['updates'], 0)
            self.assertEqual(mixed['counts'].get('failed', 0), 0)
            self.assertEqual(adapter.delete('000000000005'), 1)
            self.assertEqual(adapter.count(), 30)
            self.assertIsNone(adapter.read_by_id('000000000005'))
            with self.assertRaises(NotSupported):
                adapter.scan()
        finally:
            if prepared:
                adapter.cleanup()
            adapter.disconnect()

    @unittest.skipUnless(os.environ.get('SCALE_LAB_TEST_NATIVE') == '1', 'native integration opt-in')
    def test_nexoradb(self):
        with tempfile.TemporaryDirectory(prefix='nexora-scale-lab-') as path:
            self.exercise(NexoraAdapter(path))

    @unittest.skipUnless(os.environ.get('SCALE_LAB_POSTGRES_DSN'), 'PostgreSQL DSN not configured')
    def test_postgresql(self):
        self.exercise(PostgreSQLAdapter(os.environ['SCALE_LAB_POSTGRES_DSN'],
                                       'scale_lab_test_' + uuid.uuid4().hex))

    @unittest.skipUnless(os.environ.get('SCALE_LAB_MONGO_URI'), 'MongoDB URI not configured')
    def test_mongodb(self):
        self.exercise(MongoDBAdapter(os.environ['SCALE_LAB_MONGO_URI'],
                                    'scale_lab_test_' + uuid.uuid4().hex))
