#!/usr/bin/env python3
"""Local read-path stage probe; ASGI/TestClient is NOT network throughput."""
import argparse
import json
from pathlib import Path
import statistics
import sys
import tempfile
import time
import tracemalloc
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "NexoraDB/src"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=5000)
    parser.add_argument("--repetitions", type=int, default=10)
    args = parser.parse_args()
    if not 1 <= args.rows <= 100000 or not 3 <= args.repetitions <= 100:
        parser.error("rows 1..100000; repetitions 3..100")
    import nexoradb
    from fastapi import FastAPI
    from fastapi.testclient import TestClient
    from nexoradb.api.app import create_api_router
    from nexoradb.api.security import require_app_token
    from nexoradb_admin.config import AdminApiSettings

    def measure(action):
        action()  # warm-up, excluded
        timings = []
        tracemalloc.start()
        for _ in range(args.repetitions):
            started = time.perf_counter()
            action()
            timings.append((time.perf_counter() - started) * 1000)
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        return dict(median_ms=statistics.median(timings), min_ms=min(timings),
                    max_ms=max(timings), python_tracemalloc_peak_bytes=peak)

    with tempfile.TemporaryDirectory(prefix="nexora-path-") as directory:
        engine = nexoradb.DocEngine(directory + "/db")
        assert engine.create_collection("items").success
        for index in range(args.rows):
            assert engine.insert_one("items", json.dumps({"_id": str(index), "value": index, "text": "x" * 64})).success
        app = FastAPI()
        app.state.settings = AdminApiSettings(db_path=Path(directory) / "db")
        # Isolate data conversion costs; authentication cost excluded here.
        app.dependency_overrides[require_app_token] = lambda: SimpleNamespace(
            managed=False, scopes=["query:execute", "documents:read"], app_id="probe", token_id="probe")
        app.include_router(create_api_router(engine_provider=lambda: engine, graph_manager_provider=lambda: None))
        def native():
            result = engine.find_many("items", limit=args.rows)
            assert result.success
            return result.data
        def decoded():
            assert len(json.loads(native())) == args.rows
        with TestClient(app) as client:
            def query(raw):
                response = client.post("/api/v1/query", json={"query": "SELECT * FROM items;", "include_raw": raw})
                assert response.status_code == 200, response.text
                assert len(response.json()["rows"]) == args.rows
            def streamed():
                # TestClient buffers the body; this is not a peak-memory test
                # for an actual slow streaming network consumer.
                response = client.get("/api/v1/collections/items/documents/stream")
                assert response.status_code == 200
                assert len(response.content.splitlines()) == args.rows + 1
            report = dict(rows=args.rows, repetitions=args.repetitions,
                          build=nexoradb.build_info(), platform=sys.platform,
                          scope="warm local reads; tracemalloc overhead included; native allocations excluded; ASGI bypasses auth/network/TLS",
                          native_json=measure(native), python_decode=measure(decoded),
                          asgi_query_raw=measure(lambda: query(True)),
                          asgi_query_compact=measure(lambda: query(False)),
                          asgi_document_stream=measure(streamed))
        assert engine.prepare_shutdown().success
        del engine
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
