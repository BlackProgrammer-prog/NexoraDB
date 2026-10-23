#!/usr/bin/env python3
"""Real loopback TCP + authentication + pooled driver/native read soak.

Creates only a disposable DB; never opens a production path. Not a WAN/TLS,
failover, write-mix, or HTTP/2/3 certification. Bounded sample reservoir.
"""
import argparse
from collections import Counter, deque
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "NexoraDB/src"))


def rss():
    for line in Path("/proc/self/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--connections", type=int, default=2)
    parser.add_argument("--rows", type=int, default=5000)
    parser.add_argument("--max-rss-growth-mib", type=int, default=64)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 86400 or not 1 <= args.connections <= 2 or args.rows < 200:
        parser.error("seconds 1..86400; connections 1..2 (per-app cursor cap); rows >= 200")
    import nexoradb
    import uvicorn
    from fastapi import FastAPI
    from nexoradb.api.app import create_api_router
    from nexoradb.api.driver import NexoraDBClient
    from nexoradb.api.security import create_app_token
    from nexoradb_admin.admission import AdmissionMiddleware
    from nexoradb_admin.config import AdminApiSettings

    with tempfile.TemporaryDirectory(prefix="nexora-network-soak-") as directory:
        engine = nexoradb.DocEngine(directory + "/db")
        assert engine.create_collection("items").success
        for start in range(0, args.rows, 500):
            records = [json.dumps({"_id": str(i), "value": i, "padding": "x" * 64})
                       for i in range(start, min(start + 500, args.rows))]
            assert engine.insert_many("items", records).success
        settings = AdminApiSettings(db_path=Path(directory) / "db", environment="test")
        issued = create_app_token(app_id="soak", app_name="soak", scopes=["query:execute", "documents:read"],
                                  secret=settings.api_token_secret, collections={"items": ["read"]})
        assert engine.create_internal_app_token(issued.tokenId, json.dumps({"id": issued.tokenId})).success
        app = FastAPI()
        app.state.settings = settings
        app.add_middleware(AdmissionMiddleware)
        app.include_router(create_api_router(engine_provider=lambda: engine, graph_manager_provider=lambda: None))
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sock.listen(128)
        port = sock.getsockname()[1]
        server = uvicorn.Server(uvicorn.Config(app, access_log=False, log_level="error", http="h11"))
        thread = threading.Thread(target=lambda: server.run(sockets=[sock]), daemon=True)
        thread.start()
        for _ in range(100):
            if server.started:
                break
            time.sleep(.05)
        if not server.started:
            raise RuntimeError("test server startup failed")
        lock = threading.Lock()
        counts, errors = Counter(), Counter()
        latencies = deque(maxlen=8192)
        initial_rss = peak_rss = rss()
        initial_fds = len(list(Path("/proc/self/fd").iterdir()))
        started = time.monotonic()
        deadline = started + args.seconds
        with NexoraDBClient(url=f"http://127.0.0.1:{port}", token=issued.token,
                            max_connections=args.connections, pool_timeout=2) as client:
            def worker():
                sequence = 0
                while time.monotonic() < deadline:
                    sequence += 1
                    mode = "cursor" if sequence % 10 == 0 else "count"
                    before = time.monotonic()
                    try:
                        if mode == "cursor":
                            with client.cursor("SELECT value FROM items WHERE value >= $minimum LIMIT 100;",
                                               {"minimum": args.rows // 2}, batch_size=64) as cursor:
                                rows = list(cursor)
                                assert len(rows) == 100 and all(set(row) == {"_id", "value"} for row in rows)
                        else:
                            result = client.execute("COUNT FROM items;", include_raw=False)
                            assert result.rows[0]["count"] == args.rows
                        with lock:
                            counts[mode] += 1
                            latencies.append((time.monotonic() - before) * 1000)
                    except Exception as exc:
                        with lock:
                            errors[type(exc).__name__] += 1
                        time.sleep(.02)
            try:
                with ThreadPoolExecutor(args.connections) as pool:
                    futures = [pool.submit(worker) for _ in range(args.connections)]
                    while any(not future.done() for future in futures):
                        peak_rss = max(peak_rss, rss())
                        time.sleep(1)
                    for future in futures:
                        future.result()
                elapsed = time.monotonic() - started
                after_rss = rss()
                ordered = sorted(latencies)
                passed = not errors and sum(counts.values()) > 0 and after_rss - initial_rss <= args.max_rss_growth_mib * 1024 * 1024
                report = dict(seconds=elapsed, operations=dict(counts), errors=dict(errors),
                              ops_per_second=sum(counts.values()) / elapsed,
                              recent_samples=len(ordered), recent_p50_ms=ordered[len(ordered)//2] if ordered else None,
                              recent_p99_ms=ordered[min(len(ordered)-1, int(len(ordered)*.99))] if ordered else None,
                              process_initial_rss=initial_rss, process_final_rss=after_rss, process_peak_rss=peak_rss,
                              initial_fds=initial_fds, active_fds=len(list(Path("/proc/self/fd").iterdir())),
                              pool_connections=args.connections, passed=passed,
                              pool_stats=client.pool_stats(),
                              scope="single process client+server; real loopback HTTP/1.1, authenticated native reads; no TLS/WAN/write mix",
                              build=nexoradb.build_info())
            finally:
                server.should_exit = True
                thread.join(10)
                sock.close()
                if thread.is_alive():
                    raise RuntimeError("server did not shut down")
        assert engine.prepare_shutdown().success
        del engine
    print(json.dumps(report, indent=2), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
