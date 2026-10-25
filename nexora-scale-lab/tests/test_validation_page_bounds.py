import unittest
from unittest.mock import Mock
from scale_lab.adapters import NexoraAdapter


class ValidationPageBoundsTests(unittest.TestCase):
    def test_large_load_batches_do_not_increase_validation_page_size(self):
        adapter = NexoraAdapter('/unused', 'scale_lab_items')
        stream = Mock()
        stream.__iter__ = Mock(return_value=iter([]))
        adapter.engine = Mock()
        adapter.engine.iter_documents.return_value = stream
        list(adapter.iter_records(batch_size=10000))
        adapter.engine.iter_documents.assert_called_once_with('scale_lab_items', page_size=1000)
        stream.close.assert_called_once()
