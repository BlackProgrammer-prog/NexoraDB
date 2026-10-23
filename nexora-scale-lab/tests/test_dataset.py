import unittest

from scale_lab.dataset import DatasetConfig, batches, canonical, fingerprint, record


class DatasetTests(unittest.TestCase):
    def test_batch_size_does_not_change_fingerprint(self):
        self.assertEqual(fingerprint(DatasetConfig(137, 42, 7)),
                         fingerprint(DatasetConfig(137, 42, 64)))

    def test_partitions_reconstruct_stream(self):
        config = DatasetConfig(137, 42, 13)
        whole = [r for b in batches(config) for r in b]
        partitions = [r for start, stop in [(0, 49), (49, 100), (100, 137)]
                      for b in batches(config, start, stop) for r in b]
        self.assertEqual(whole, partitions)
        self.assertEqual(len({r['id'] for r in whole}), 137)
        self.assertTrue(all(len(b) <= 13 for b in batches(config)))

    def test_seed_and_random_access(self):
        config = DatasetConfig(100)
        self.assertEqual(record(config, 99), record(config, 99))
        self.assertNotEqual(record(config, 99), record(DatasetConfig(100, 1), 99))
        self.assertIsInstance(record(config, 99)['balance_cents'], int)
        self.assertTrue(canonical(record(config, 99)).endswith(b'\n'))

    def test_invalid_input(self):
        for kwargs in ({'records': 0}, {'records': -1}, {'batch_size': 0}, {'seed': True}):
            with self.assertRaises(ValueError):
                DatasetConfig(**kwargs)
        with self.assertRaises(ValueError):
            record(DatasetConfig(10), 10)
        with self.assertRaises(ValueError):
            list(batches(DatasetConfig(10), 5, 11))


if __name__ == '__main__':
    unittest.main()
