import threading
import unittest
from unittest.mock import patch

from scale_lab.dataset import DatasetConfig, record
from scale_lab.integrity import ContentDigest
from scale_lab.loading import load_batches


class LoadingTests(unittest.TestCase):
    def test_prefetch_matches_serial_and_snapshots_never_advance(self):
        config = DatasetConfig(37, batch_size=5)
        results = []
        for depth in (0, 1, 2):
            observed = []
            baseline = ContentDigest()
            with load_batches(config, depth) as prepared:
                for batch, digest in prepared:
                    for value in batch:
                        baseline.add(value)
                    self.assertEqual(digest.result(), baseline.result())
                    observed.append((batch, digest.result()))
            results.append(observed)
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0], results[2])
        self.assertEqual(results[0][0][1]['records'], 5)

    def test_producer_failure_is_propagated(self):
        def broken(_):
            yield [record(DatasetConfig(10), 0)]
            raise RuntimeError('controlled producer failure')
        with patch('scale_lab.loading.batches', broken):
            with self.assertRaisesRegex(RuntimeError, 'controlled producer failure'):
                with load_batches(DatasetConfig(10, batch_size=1), 2) as prepared:
                    list(prepared)

    def test_early_stop_and_write_failure_join_producer(self):
        for fail in (False, True):
            try:
                with load_batches(DatasetConfig(10000), 1) as prepared:
                    next(prepared)
                    if fail:
                        raise RuntimeError('controlled acknowledgment failure')
            except RuntimeError:
                self.assertTrue(fail)
            self.assertFalse(any(t.name == 'scale-lab-load-producer' for t in threading.enumerate()))

    def test_rejects_unbounded_or_invalid_prefetch(self):
        for depth in (-1, 9, True, 1.5):
            with self.assertRaises(ValueError):
                with load_batches(DatasetConfig(1), depth):
                    pass
