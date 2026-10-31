import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scale_lab import memory_control as memory
from scale_lab.memory_diagnostics import python_memory
from scale_lab import memory_diagnostics as diagnostics


class MemoryDiagnosticsTests(unittest.TestCase):
    def test_combined_sampler_is_observation_only(self):
        import os
        from scale_lab.resources import Sampler
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(diagnostics, 'reclaim_allocator', side_effect=AssertionError('hidden trim')), \
                patch.object(memory, 'host_memory', return_value={}), \
                patch.object(memory, 'group_snapshot', return_value={}):
            with Sampler(Path(directory) / 'sample.jsonl', [os.getpid()], interval=60, combined=True) as sampler:
                pass
            self.assertGreaterEqual(sampler.samples, 2)

    def test_trim_threshold_and_cooldown(self):
        from unittest.mock import Mock
        free = {'glibc': {'arena_bytes': 2*1024**3, 'free_arena_bytes': 1024**3}}
        trim = Mock(return_value=1)
        library = Mock(malloc_trim=trim)
        with patch.object(diagnostics, 'python_memory', return_value=free), \
                patch.object(diagnostics.ctypes, 'CDLL', return_value=library), \
                patch.object(diagnostics, '_last_trim', float('-inf')):
            self.assertTrue(diagnostics.reclaim_allocator()['attempted'])
            self.assertEqual(diagnostics.reclaim_allocator()['reason'], 'rate_limited')
            self.assertTrue(diagnostics.reclaim_allocator(force=True)['attempted'])
            self.assertEqual(trim.call_count, 2)

    def test_trim_skips_live_or_unsupported_allocators(self):
        for value in (None, {'arena_bytes': 2*1024**3, 'free_arena_bytes': 100*1024**2},
                      {'arena_bytes': 8*1024**3, 'free_arena_bytes': 512*1024**2}):
            with patch.object(diagnostics, 'python_memory', return_value={'glibc': value}), \
                    patch.object(diagnostics.ctypes, 'CDLL') as load, \
                    patch.object(diagnostics, '_last_trim', float('-inf')):
                self.assertFalse(diagnostics.reclaim_allocator()['attempted'])
                load.assert_not_called()

    def test_trim_failure_releases_maintenance_lock(self):
        from unittest.mock import Mock
        free = {'glibc': {'arena_bytes': 2*1024**3, 'free_arena_bytes': 1024**3}}
        with patch.object(diagnostics, 'python_memory', return_value=free), \
                patch.object(diagnostics.ctypes, 'CDLL', return_value=Mock(malloc_trim=Mock(side_effect=RuntimeError('fault')))), \
                patch.object(diagnostics, '_last_trim', float('-inf')):
            with self.assertRaises(RuntimeError):
                diagnostics.reclaim_allocator(force=True)
        self.assertTrue(diagnostics._trim_lock.acquire(blocking=False))
        diagnostics._trim_lock.release()

    def test_allocator_report_does_not_enable_tracing(self):
        import tracemalloc
        before = tracemalloc.is_tracing()
        value = python_memory()
        self.assertEqual(tracemalloc.is_tracing(), before)
        self.assertGreaterEqual(value['allocated_blocks'], 0)
        if value['glibc'] is not None:
            self.assertGreaterEqual(value['glibc']['free_arena_bytes'], 0)

    def test_disabled_cleanup_never_accesses_cgroup(self):
        with patch.object(memory, 'ROOT', Path('/does-not-exist')):
            with memory.cleanup_headroom(False):
                pass

    def test_cleanup_restores_soft_threshold_even_on_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            group = root / memory.GROUPS['worker']
            group.mkdir()
            (group / 'memory.high').write_text(str(memory.HIGH))
            (group / 'memory.max').write_text(str(memory.MAXIMUM))
            original_read = Path.read_text
            def read(path, *args, **kwargs):
                if str(path) == '/proc/self/cgroup':
                    return '0::/' + memory.GROUPS['worker'] + '\n'
                return original_read(path, *args, **kwargs)
            with patch.object(memory, 'ROOT', root), patch.object(Path, 'read_text', read):
                with self.assertRaisesRegex(ValueError, 'test failure'):
                    with memory.cleanup_headroom(True):
                        self.assertEqual((group / 'memory.high').read_text(), str(memory.MAXIMUM))
                        raise ValueError('test failure')
            self.assertEqual((group / 'memory.high').read_text(), str(memory.HIGH))
            self.assertEqual((group / 'memory.max').read_text(), str(memory.MAXIMUM))
