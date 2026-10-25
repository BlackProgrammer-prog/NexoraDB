import unittest
from bson import BSON
from scale_lab.dataset import DatasetConfig,record
from scale_lab.integrity import ContentDigest
from scale_lab.encoded_loading import encoded_batches


class EncodedLoadingTests(unittest.TestCase):
    def test_spawned_pipeline_preserves_order_and_digest(self):
        config=DatasetConfig(31,batch_size=7)
        expected=ContentDigest()
        for i in range(config.records): expected.add(record(config,i))
        values=[]
        with encoded_batches(config,2) as prepared:
            for encoded,digest in prepared:
                values.extend(BSON(value).decode() for value in encoded)
        self.assertEqual(digest.result(),expected.result())
        for i,value in enumerate(values):
            self.assertEqual(value.pop('_id'),f'{i:012d}')
            self.assertEqual(value,record(config,i))

    def test_early_stop_shuts_down_spawned_workers(self):
        with encoded_batches(DatasetConfig(23,batch_size=3),2) as prepared:
            encoded,_=next(prepared)
            self.assertEqual(len(encoded),3)

    def test_worker_count_is_bounded(self):
        for value in (0,5,True):
            with self.assertRaises(ValueError):
                with encoded_batches(DatasetConfig(1),value): pass
