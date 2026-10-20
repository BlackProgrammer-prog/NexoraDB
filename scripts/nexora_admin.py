#!/usr/bin/env python3
"""Offline POSIX administration. Requires the matching nexoradb native module."""
import argparse
import contextlib
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time


def result(value):
    if not value.success:
        raise RuntimeError(value.error_msg)
    return json.loads(value.data) if value.data else None


def emit(event, **values):
    print(json.dumps(dict(event=event, **values)), flush=True)


def regular_files(root):
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise RuntimeError(f"symlink/special file rejected: {path}")
        if path.is_file():
            yield path


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return dict(size=path.stat().st_size, sha256=value.hexdigest())


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def sync_tree(root):
    for path in regular_files(root):
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    directories = [p for p in root.rglob("*") if p.is_dir()]
    for path in sorted(directories, key=lambda p: len(p.parts), reverse=True):
        sync_directory(path)
    sync_directory(root)


def publish_new(stage, target):
    # Atomic no-replace publication, including a concurrent target-creation race.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    if rename(-100, os.fsencode(stage), -100, os.fsencode(target), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(target))
    sync_directory(target.parent)


def disjoint(destination, *sources):
    for source in sources:
        if destination == source or destination in source.parents or source in destination.parents:
            raise ValueError("source and destination trees must be disjoint")


@contextlib.contextmanager
def graph_lease(root):
    if not root.is_dir():
        raise ValueError("existing graph directory required")
    fd = os.open(root / ".nexora-admin.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Also exclude standalone GraphStorage writers on every existing graph.
        with contextlib.ExitStack() as stack:
            for path in sorted(root.rglob("*.storage.lock")):
                if path.is_symlink():
                    raise RuntimeError("symlink graph lock rejected")
                lock = stack.enter_context(path.open("r+b"))
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield
    finally:
        os.close(fd)


@contextlib.contextmanager
def readonly_db_lease(path):
    # RocksDB's POSIX LOCK uses fcntl record locking, not flock.
    with (path / "LOCK").open("r+b") as lock:
        fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def check_database(path):
    if not (path / "CURRENT").is_file():
        raise ValueError("existing RocksDB CURRENT required; refusing to create an empty source")


def validate(root, rebuild=False):
    import nexoradb
    engine = nexoradb.DocEngine(str(root / "documents"))
    engine.pause_document_migration()
    manager = None
    try:
        checked = result(engine.integrity_check())
        manager = nexoradb.GraphManager(engine, str(root / "graphs"))
        if rebuild:
            for definition in (root / "graphs").glob("*.graphdef"):
                parsed = json.loads(definition.read_text(encoding="utf-8"))
                if parsed.get("format_version") != 2 or parsed.get("name") != definition.stem:
                    raise RuntimeError("unknown or mismatched graph definition; refusing silent omission")
            if not manager.startup():
                raise RuntimeError("graph definition startup failed")
            for name in manager.list_graphs():
                built = manager.build_graph(name)
                if not built.success:
                    raise RuntimeError(built.error_msg)
                emit("graph_rebuilt", graph=name, nodes=built.nodes_built, edges=built.edges_built)
        elif not manager.recover_stored_graphs():
            raise RuntimeError("stored graph recovery failed")
        graphs = {}
        for name in manager.list_graphs():
            stats = manager.get_stats(name)
            snapshot = manager.create_snapshot(name)
            fingerprint = hashlib.sha256()
            def visit_node(dense_id, type_id):
                fingerprint.update(json.dumps([dense_id, snapshot.ext_id(dense_id), type_id]).encode() + b"\n")
                return True
            snapshot.for_each_node(visit_node)
            for edge in snapshot.iter_edges():
                fingerprint.update(json.dumps(edge).encode() + b"\n")
            graphs[name] = dict(nodes=stats.active_nodes, edges=stats.active_edges,
                                traversal_sha256=fingerprint.hexdigest())
            del snapshot
        return dict(documents=checked, graphs=graphs, formats=result(engine.format_versions()))
    finally:
        if manager is not None:
            manager.shutdown()
            del manager
        del engine


def write_manifest(stage, report, checkpoint):
    files = {str(path.relative_to(stage)): digest(path) for path in regular_files(stage)}
    manifest = dict(bundle_version=1, consistency="offline-quiescent-document-and-graph",
                    checkpoint=checkpoint, report=report, files=files)
    (stage / "bundle.json").write_text(json.dumps(manifest, sort_keys=True, indent=2), encoding="utf-8")
    sync_tree(stage)


def verify_bundle(root):
    if (root / "bundle.json").is_symlink():
        raise RuntimeError("symlink manifest rejected")
    manifest = json.loads((root / "bundle.json").read_text(encoding="utf-8"))
    if manifest["bundle_version"] != 1 or manifest["consistency"] != "offline-quiescent-document-and-graph":
        raise RuntimeError("unknown bundle version/consistency")
    actual = {str(path.relative_to(root)): digest(path) for path in regular_files(root)
              if path != root / "bundle.json"}
    if actual != manifest["files"]:
        raise RuntimeError("bundle checksum/file-set mismatch")
    # Exact enumeration, not joining manifest-controlled paths (no ../ escape).
    if not (root / "documents" / "CURRENT").is_file() or not (root / "graphs").is_dir():
        raise RuntimeError("incomplete bundle")
    return manifest


def new_stage(target):
    if target.exists():
        raise FileExistsError(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=target.name + ".staging-", dir=target.parent))


def backup(db, graphs, target, rebuild=False):
    import nexoradb
    check_database(db)
    disjoint(target, db, graphs)
    stage = new_stage(target)
    try:
        with graph_lease(graphs):
            engine = nexoradb.DocEngine(str(db))  # RocksDB LOCK excludes other document processes.
            engine.pause_document_migration()
            try:
                checkpoint = result(engine.create_checkpoint(str(stage / "documents")))
                list(regular_files(graphs))  # Reject links before copytree follows anything.
                if rebuild:
                    (stage / "graphs").mkdir()
                    for definition in graphs.glob("*.graphdef"):
                        shutil.copy2(definition, stage / "graphs" / definition.name)
                else:
                    shutil.copytree(graphs, stage / "graphs")
            finally:
                del engine
        report = validate(stage, rebuild)
        write_manifest(stage, report, checkpoint)
        publish_new(stage, target)
        return report
    except Exception:
        emit("operation_failed", staging=str(stage), staging_exists=stage.exists(), target_exists=target.exists())
        raise


def restore(bundle, target):
    disjoint(target, bundle)
    started = time.monotonic()
    original = verify_bundle(bundle)
    stage = new_stage(target)
    try:
        for name in ("documents", "graphs"):
            shutil.copytree(bundle / name, stage / name)  # Copy, never link writable restored SSTs.
        shutil.copy2(bundle / "bundle.json", stage / "bundle.json")
        verify_bundle(stage)
        report = validate(stage)
        if report != original["report"]:
            raise RuntimeError("recovered counts/formats differ from backup report")
        # Recovery can normalize WAL/LOG files. Refresh checksums after validation.
        (stage / "bundle.json").unlink()
        write_manifest(stage, report, original["checkpoint"])
        publish_new(stage, target)
        return dict(elapsed_seconds=time.monotonic() - started, report=report)
    except Exception:
        emit("operation_failed", staging=str(stage), staging_exists=stage.exists(), target_exists=target.exists())
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("backup", "rebuild-graphs"):
        item = sub.add_parser(command)
        item.add_argument("--db", type=Path, required=True)
        item.add_argument("--graphs", type=Path, required=True)
        item.add_argument("--output", type=Path, required=True)
    item = sub.add_parser("restore")
    item.add_argument("--bundle", type=Path, required=True)
    item.add_argument("--output", type=Path, required=True)
    for command in ("inspect", "integrity", "migrate", "rebuild-indexes"):
        item = sub.add_parser(command)
        item.add_argument("--db", type=Path, required=True)
        if command == "migrate":
            item.add_argument("--dry-run", action="store_true")
        if command == "rebuild-indexes":
            item.add_argument("--collection", required=True)
    args = parser.parse_args(argv)
    for field in ("db", "graphs", "bundle", "output"):
        if hasattr(args, field):
            setattr(args, field, getattr(args, field).resolve())
    import nexoradb
    if args.command in ("backup", "rebuild-graphs"):
        emit("backup_complete", report=backup(args.db, args.graphs, args.output, args.command == "rebuild-graphs"))
    elif args.command == "restore":
        emit("restore_complete", **restore(args.bundle, args.output))
    else:
        check_database(args.db)
        if args.command == "inspect" or args.command == "migrate" and args.dry_run:
            with readonly_db_lease(args.db):
                emit("dry_run", report=result(nexoradb.DocEngine.inspect_store(str(args.db))))
            return
        engine = nexoradb.DocEngine(str(args.db))
        engine.pause_document_migration()
        if args.command == "integrity":
            emit("integrity", report=result(engine.integrity_check()))
        elif args.command == "rebuild-indexes":
            result(engine.rebuild_indexes(args.collection))
            emit("indexes_rebuilt", collection=args.collection)
        elif args.command == "migrate":
            migrated = 0
            while True:
                progress = result(engine.migrate_legacy_documents())
                migrated += progress["migrated"]
                emit("migration_progress", total_migrated=migrated, **progress)
                if progress["complete"]:
                    break
            for collection in engine.list_collections():
                result(engine.rebuild_indexes(collection))
                emit("index_migration_progress", collection=collection)
            result(engine.integrity_check())
            emit("migration_complete", versions=result(engine.upgrade_format_manifest()))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        emit("error", error=str(error))
        sys.exit(1)
