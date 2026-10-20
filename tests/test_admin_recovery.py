"""Run with PYTHONPATH=build/debug python3 tests/test_admin_recovery.py."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import random
import base64
import shutil

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("nexora_admin", ROOT / "scripts/nexora_admin.py")
admin = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(admin)


def fixture(root, records=100, random_payload=False):
    import nexoradb
    root.mkdir(parents=True)
    generator = random.Random(20261020)
    db = nexoradb.DocEngine(str(root / "documents"))
    for collection in ("users", "links"):
        assert db.create_collection(collection).success
    for start in range(0, records, 500):
        users, links = [], []
        for i in range(start, min(start + 500, records)):
            padding = base64.b64encode(generator.randbytes(768)).decode() if random_payload else (str(i) + "x") * 100
            users.append(json.dumps(dict(_id=str(i), value=i, padding=padding)))
            links.append(json.dumps(dict(_id=str(i), src=str(i), dst=str((i + 1) % records))))
        assert db.insert_many("users", users).success
        assert db.insert_many("links", links).success
    index = nexoradb.IndexDefinition()
    index.index_name, index.fields = "value", ["value"]
    assert db.create_index("users", index).success
    manager = nexoradb.GraphManager(db, str(root / "graphs"))
    definition = nexoradb.GraphDefinition()
    definition.name = "social"
    node = nexoradb.NodeMappingDef()
    node.node_type, node.collection, node.key_path = "User", "users", "_id"
    edge = nexoradb.EdgeMappingDef()
    edge.edge_type, edge.collection = "Link", "links"
    edge.source_path, edge.target_path = "src", "dst"
    edge.source_node_type = edge.target_node_type = "User"
    definition.node_mappings.append(node)
    definition.edge_mappings.append(edge)
    assert manager.create_graph(definition)
    built = manager.build_graph("social")
    assert built.success, built.error_msg
    manager.shutdown()
    del manager, db


class AdminRecovery(unittest.TestCase):
    def command(self, *args, success=True):
        completed = subprocess.run([sys.executable, str(ROOT / "scripts/nexora_admin.py"), *map(str, args)],
                                   text=True, capture_output=True, timeout=90)
        self.assertEqual(completed.returncode == 0, success, completed.stdout + completed.stderr)
        return completed

    @unittest.skipUnless(os.environ.get("LEGACY_READER_PROBE"), "archived pre-phase-8 binary not supplied")
    def test_actual_archived_reader_upgrade_rejection_and_checkpoint_rollback(self):
        import nexoradb
        with tempfile.TemporaryDirectory(prefix="nexora_old_reader_") as temporary:
            root = Path(temporary)
            fixture(root / "source")
            db = nexoradb.DocEngine(str(root / "source/documents"))
            self.assertTrue(db.create_checkpoint(str(root / "old")).success)
            del db
            reader = os.environ["LEGACY_READER_PROBE"]
            opened = subprocess.run([reader, str(root / "old")], capture_output=True, timeout=20)
            self.assertEqual(opened.returncode, 0, opened.stderr)
            self.command("migrate", "--db", root / "source/documents")
            rejected = subprocess.run([reader, str(root / "source/documents")], capture_output=True, timeout=20)
            self.assertEqual(rejected.returncode, 2, rejected.stderr)
            self.assertIn(b"Unsupported document format version: 2", rejected.stderr)
            rollback = subprocess.run([reader, str(root / "old")], capture_output=True, timeout=20)
            self.assertEqual(rollback.returncode, 0, rollback.stderr)

    def test_backup_restore_rebuild_checksums_and_no_overwrite(self):
        import nexoradb
        with tempfile.TemporaryDirectory(prefix="nexora_admin_") as temporary:
            root = Path(temporary)
            source, bundle, restored = root / "source", root / "bundle", root / "restored"
            fixture(source)
            # Cross-process backup must fail while a manager owns the root lease.
            db = nexoradb.DocEngine(str(source / "documents"))
            manager = nexoradb.GraphManager(db, str(source / "graphs"))
            self.command("backup", "--db", source / "documents", "--graphs", source / "graphs",
                         "--output", root / "busy", success=False)
            manager.shutdown()
            del manager, db
            before = {str(p.relative_to(source)): admin.digest(p) for p in admin.regular_files(source)}
            self.command("migrate", "--dry-run", "--db", source / "documents")
            after = {str(p.relative_to(source)): admin.digest(p) for p in admin.regular_files(source)}
            self.assertEqual(before, after, "dry run modified source bytes")
            self.command("backup", "--db", source / "documents", "--graphs", source / "graphs", "--output", bundle)
            self.command("restore", "--bundle", bundle, "--output", restored)
            report = admin.verify_bundle(restored)["report"]
            self.assertEqual(report["graphs"]["social"]["nodes"], 100)
            self.assertEqual(report["graphs"]["social"]["edges"], 100)
            self.assertEqual(report["documents"]["documents"], 200)
            self.command("restore", "--bundle", bundle, "--output", restored, success=False)
            self.command("migrate", "--db", restored / "documents")
            self.command("inspect", "--db", restored / "documents")
            self.command("rebuild-graphs", "--db", source / "documents", "--graphs", source / "graphs",
                         "--output", root / "rebuilt")
            self.assertEqual(admin.verify_bundle(root / "rebuilt")["report"]["graphs"], report["graphs"])
            bad_graph = root / "bad_graph"
            shutil.copytree(bundle, bad_graph)
            manifest = next((bad_graph / "graphs/social").glob("*.manifest"))
            manifest.write_bytes(b"unsupported graph manifest")
            description = json.loads((bad_graph / "bundle.json").read_text())
            description["files"][str(manifest.relative_to(bad_graph))] = admin.digest(manifest)
            (bad_graph / "bundle.json").write_text(json.dumps(description))
            self.command("restore", "--bundle", bad_graph, "--output", root / "bad_graph_restore", success=False)
            self.assertFalse((root / "bad_graph_restore").exists())
            sst = next((bundle / "documents").glob("*.sst"))
            with sst.open("r+b") as stream:
                first = stream.read(1)
                stream.seek(0)
                stream.write(bytes([first[0] ^ 1]))
            self.command("restore", "--bundle", bundle, "--output", root / "corrupt", success=False)
            self.assertFalse((root / "corrupt").exists())


if __name__ == "__main__":
    unittest.main()
