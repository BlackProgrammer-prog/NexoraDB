import unittest
from unittest.mock import Mock
from scale_lab.adapters import NexoraAdapter
from scale_lab.integrity import full_validate
from scale_lab.dataset import DatasetConfig
from scale_lab.dataset import record
from scale_lab.pagination import DEFAULT_PAGE_BYTES


class ValidationPageBoundsTests(unittest.TestCase):
    def test_expired_scan_never_returns_successful_partial_validation(self):
        adapter = NexoraAdapter('/unused', 'scale_lab_items')
        config = DatasetConfig(records=2, batch_size=1)
        adapter.count = Mock(return_value=2)
        class ExpiringStream:
            closed = False
            def __iter__(self):
                yield record(config, 0)
                raise RuntimeError('Expired or invalid continuation token')
            def close(self): self.closed = True
        stream = ExpiringStream()
        adapter.engine = Mock()
        adapter.engine.iter_documents.return_value = stream
        with self.assertRaisesRegex(RuntimeError, 'Expired'):
            full_validate(adapter, config)
        self.assertTrue(stream.closed)

    def test_large_load_batches_do_not_increase_validation_page_size(self):
        adapter = NexoraAdapter('/unused', 'scale_lab_items')
        stream = Mock()
        stream.__iter__ = Mock(return_value=iter([]))
        adapter.engine = Mock()
        adapter.engine.iter_documents.return_value = stream
        config = DatasetConfig(records=1, batch_size=10000)
        adapter.count = Mock(return_value=1)
        adapter.iter_records = Mock(return_value=iter([]))
        with self.assertRaises(RuntimeError):  # Empty fake scan; still verifies page-size routing.
            full_validate(adapter, config)
        adapter.iter_records.assert_called_once_with(1000)

    def test_adapter_rejects_invalid_scan_before_native_acquisition(self):
        adapter = NexoraAdapter('/unused', 'scale_lab_items')
        adapter.engine = Mock()
        for size in (0, -1, 1001, 4096, 10000, True, 2.5):
            with self.assertRaises(ValueError):
                list(adapter.iter_records(size))
        adapter.engine.iter_documents.assert_not_called()

    def test_valid_scan_uses_explicit_budgets_and_closes(self):
        adapter = NexoraAdapter('/unused', 'scale_lab_items')
        stream = Mock()
        stream.__iter__ = Mock(return_value=iter([]))
        adapter.engine = Mock()
        adapter.engine.iter_documents.return_value = stream
        list(adapter.iter_records(1000))
        adapter.engine.iter_documents.assert_called_once_with('scale_lab_items', page_size=1000, max_page_bytes=DEFAULT_PAGE_BYTES)
        stream.close.assert_called_once()
