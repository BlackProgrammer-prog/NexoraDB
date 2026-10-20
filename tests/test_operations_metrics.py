import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import os
SPEC = importlib.util.spec_from_file_location("operations_metrics", Path(__file__).resolve().parents[1] / "scripts/operations_metrics.py")
metrics = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(metrics)


class LiveMetrics(unittest.TestCase):
    @unittest.skipIf(os.geteuid() == 0, "POSIX permission enforcement requires unprivileged UID")
    def test_real_permission_denied_checkpoint_keeps_source_usable(self):
        import nexoradb
        with tempfile.TemporaryDirectory(prefix="nexora_permission_") as path:
            engine = nexoradb.DocEngine(str(Path(path) / "documents"))
            blocked = Path(path) / "blocked"
            blocked.mkdir()
            blocked.chmod(0o500)
            try:
                self.assertFalse(engine.create_checkpoint(str(blocked / "checkpoint")).success)
                self.assertTrue(engine.is_healthy())
            finally:
                blocked.chmod(0o700)
            del engine

    def test_native_export_health_and_graceful_checkpoint(self):
        import nexoradb
        with tempfile.TemporaryDirectory(prefix="nexora_operations_") as path:
            engine = nexoradb.DocEngine(str(Path(path) / "documents"))
            self.assertTrue(engine.create_collection("items").success)
            self.assertTrue(engine.insert_one("items", '{"_id":"a","value":1}').success)
            manager = nexoradb.GraphManager(engine, str(Path(path) / "graphs"))
            definition = nexoradb.GraphDefinition()
            definition.name = "empty"
            self.assertTrue(manager.create_graph(definition))
            sample = metrics.collect(engine, manager)
            text = metrics.prometheus(sample)
            self.assertIn("nexora_operation_duration_seconds_bucket", text)
            self.assertIn("nexora_wal_pending_entries", text)
            self.assertIn("nexora_queue_workers", text)
            self.assertTrue(sample["health"]["ready"])
            self.assertTrue(manager.prepare_shutdown())
            manager.shutdown()
            self.assertTrue(engine.prepare_shutdown().success)
            self.assertFalse(json.loads(engine.health_report())["ready"])
            del manager, engine


if __name__ == "__main__":
    unittest.main()
