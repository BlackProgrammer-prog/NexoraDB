#!/usr/bin/env python3
"""Smoke-test metadata and the native module from an installed wheel."""

from __future__ import annotations

import argparse
from importlib.metadata import version
import json
from pathlib import Path
import tempfile

import nexoradb


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected", required=True)
    arguments = parser.parse_args()
    package_version = version("NexoraDataBase")
    build_info = nexoradb.build_info()

    assert package_version == arguments.expected
    assert nexoradb.__version__ == arguments.expected
    assert build_info["project_version"] == arguments.expected
    assert build_info["build_type"] == "Release"
    # A correct metadata version alone must not hide an obsolete bundled .so.
    with tempfile.TemporaryDirectory(prefix="nexora_wheel_smoke_") as directory:
        root = Path(directory)
        pool = nexoradb.ResourceBudget(32*1024**2)
        settings = nexoradb.RocksSettings()
        settings.execution_budget = pool
        engine = nexoradb.DocEngine(str(root / "documents"), rocks_settings=settings)
        assert engine.create_collection("items").success
        assert engine.insert_one("items", '{"_id":"one","value":1}').success
        assert engine.find_by_id("items", "one").success
        assert engine.insert_one("items", '{"_id":"two","value":2}').success
        with pool.reserve('workspace',32*1024**2):
            rejected=engine.insert_one("items",'{"_id":"rejected"}')
            assert not rejected.success and rejected.error_code=='resource_exhausted'
        assert not engine.find_by_id("items","rejected").success
        scope=engine.execution_scope()
        with scope:
            scope.cancel()
            cancelled=engine.insert_one("items",'{"_id":"cancelled"}')
            assert not cancelled.success and cancelled.error_code=='execution_cancelled'
        from nexoradb.execution import QueryPipeline, SpillDiskBudget
        spill=root/'spill';spill.mkdir()
        pipe=QueryPipeline(engine,memory_bytes=1024**2,max_record_bytes=256,
                           disk_budget=SpillDiskBudget(1024**2),spill_parent=spill)
        with pipe.scan('items') as rows:
            with pipe.sort(rows,'value') as ordered:
                assert [row['value'] for row in ordered]==[1,2]
        assert not list(spill.iterdir()) and pipe.disk.report()['reserved_bytes']==0
        assert json.loads(engine.health_report())["ready"]
        assert "operations" in json.loads(engine.operation_metrics())
        assert engine.integrity_check().success
        assert engine.create_checkpoint(str(root / "checkpoint")).success
        manager = nexoradb.GraphManager(engine, str(root / "graphs"))
        definition = nexoradb.GraphDefinition()
        definition.name = "smoke"
        assert manager.create_graph(definition)
        assert manager.prepare_shutdown()
        manager.shutdown()
        assert engine.prepare_shutdown().success
        del manager, engine
        assert json.loads(pool.report())['reserved_bytes']==0
    print(
        "Installed wheel verified:",
        package_version,
        build_info["build_type"],
        build_info["rocksdb_version"],
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
