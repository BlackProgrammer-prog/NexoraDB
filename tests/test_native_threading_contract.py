"""Native binding regression; run with the build directory on PYTHONPATH.

The subprocess timeout also catches regressions that deadlock while owning GIL.
"""
import subprocess
import sys
import tempfile
import threading
import unittest


def worker():
    import nexoradb

    with tempfile.TemporaryDirectory(prefix="nexora_python_threads_") as path:
        resources = nexoradb.RocksResources(16 * 1024 * 1024)
        settings = nexoradb.RocksSettings()
        settings.shared_resources = resources
        settings.durability = nexoradb.DocumentDurability.Durable
        tuned = nexoradb.DocEngine(path + "/tuned", rocks_settings=settings)
        peer = nexoradb.DocEngine(path + "/peer", rocks_settings=settings)
        del resources, settings  # Native ownership must survive Python wrappers.
        assert tuned.create_collection("metrics").success
        assert tuned.insert_one("metrics", '{"_id":"one","value":1}').success
        assert tuned.flush_storage().success
        metrics = tuned.rocks_metrics()
        assert metrics.statistics_enabled
        assert 0 <= metrics.cache_hit_rate <= 1
        assert 0 <= metrics.bloom_useful_rate <= 1
        assert metrics.cache_capacity == peer.rocks_metrics().cache_capacity == 8 * 1024 * 1024
        assert metrics.memtable_budget == 8 * 1024 * 1024
        assert metrics.wal_write_bytes > 0 and metrics.flush_write_bytes > 0
        del tuned, peer
        engine = nexoradb.DocEngine(path)
        assert engine.create_collection("items").success
        tx = engine.begin_transaction()
        # Regression: nested gil_scoped_release here used to abort CPython.
        assert not engine.find_by_id_tx(tx, "items", "missing").success
        started, finished = threading.Event(), threading.Event()
        results = []

        def ddl():
            started.set()
            results.append(engine.create_collection("other"))
            finished.set()

        thread = threading.Thread(target=ddl)
        thread.start()
        assert started.wait(2)
        assert not finished.wait(0.05), "DDL must wait for existing transaction"
        assert engine.commit_transaction(tx).success
        assert finished.wait(5), "DDL did not resume after commit"
        thread.join()
        assert results[0].success

        # Multiple native writers share one engine while the GIL is released.
        def insert(index):
            for offset in range(50):
                result = engine.insert_one("items", '{"_id":"%s_%s"}' % (index, offset))
                if not result.success:
                    results.append(result)

        threads = [threading.Thread(target=insert, args=(i,)) for i in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        assert len(results) == 1
        assert engine.get_collection_size("items") == 200
        seen = []
        engine.iterate_collection("items", lambda key, value: seen.append(key) or True)
        assert len(seen) == 200
        def fail_callback(key, value):
            raise ValueError("callback sentinel")
        try:
            engine.iterate_collection("items", fail_callback)
            raise AssertionError("callback exception swallowed")
        except ValueError as error:
            assert str(error) == "callback sentinel"

        # Stream copies its predicate, pins the engine, closes early and keeps
        # each page bounded rather than collecting a collection-sized JSON.
        with engine.iter_documents("items", page_size=7, max_page_bytes=256) as stream:
            documents = list(stream)
        assert len(documents) == 200
        assert len({document["_id"] for document in documents}) == 200
        import json
        special = {"_id": "raw", "value": 'فارسی },{ [ ] \\"\n',
                   "nested": [{"list": [True, None, {"a": "}"}]}]}
        assert engine.insert_one("items", json.dumps(special)).success
        with engine.iter_documents("items", page_size=7, max_page_bytes=1024) as raw:
            raw_documents = []
            while True:
                chunk, count = raw.next_ndjson_batch(3)
                assert count <= 3
                lines = chunk.splitlines()
                assert len(lines) == count
                raw_documents.extend(json.loads(line)["document"] for line in lines)
                if not count:
                    break
            assert len(raw_documents) == 201
            assert special in raw_documents
            try:
                next(raw)
                raise AssertionError("mixed cursor modes accepted")
            except RuntimeError:
                pass
        assert engine.delete_by_id("items", "raw").success
        stream = engine.iter_documents("items", page_size=1)
        assert "_id" in next(stream)
        stream.close()
        assert list(stream) == []
        try:
            list(engine.iter_documents("items", max_page_bytes=2))
            raise AssertionError("oversized document accepted")
        except RuntimeError as error:
            assert "byte budget" in str(error)
        detached = engine.iter_documents("items", page_size=3)
        del stream, engine
        assert len(list(detached)) == 200
        detached.close()

        del detached
        engine = nexoradb.DocEngine(path + "/graphdb")
        options = nexoradb.SchedulerOptions()
        options.workers, options.queue_capacity = 1, 2
        manager = nexoradb.GraphManager(engine, path + "/graphs", scheduler_options=options)
        definition = nexoradb.GraphDefinition()
        definition.name = "empty"
        assert manager.create_graph(definition)
        snapshot = manager.create_snapshot("empty")
        edges = snapshot.iter_edges()
        del snapshot
        assert list(edges) == []
        assert manager.scheduler_stats().workers == 1
        result = manager.run_connected_components("empty", [])
        assert result.success
        assert result.queue_ms >= 0 and result.execution_ms >= 0
        assert result.reserved_bytes == 128 * 1024 * 1024
        tiny = nexoradb.SnapshotOptions()
        tiny.max_build_bytes = 1
        assert not manager.run_connected_components("empty", [], tiny).success
        manager.shutdown()
        del manager, engine


class NativeThreadingContract(unittest.TestCase):
    def test_gil_ddl_transaction_and_shared_engine(self):
        subprocess.run([sys.executable, __file__, "--worker"], check=True, timeout=20)


if __name__ == "__main__":
    if "--worker" in sys.argv:
        worker()
    else:
        unittest.main()
