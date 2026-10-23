import asyncio
import json
import sys
import threading
import time
import pytest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "NexoraDB/src"))

from fastapi import FastAPI
from fastapi.testclient import TestClient
from nexoradb.api.app import create_api_router
from nexoradb.api.security import create_app_token
from nexoradb_admin.admission import AdmissionMiddleware
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.monitoring import MonitoringSocketServer, MonitoringState


def test_admission_rejects_and_releases_on_failure():
    async def check():
        entered, release = asyncio.Event(), asyncio.Event()
        async def application(scope, receive, send):
            entered.set()
            await release.wait()
            raise RuntimeError("failure")
        middleware = AdmissionMiddleware(application, 1)
        async def receive():
            return {"type": "http.request"}
        sent = []
        async def send(message):
            sent.append(message)
        scope = {"type": "http", "method": "GET", "path": "/"}
        first = asyncio.create_task(middleware(scope, receive, send))
        await entered.wait()
        await middleware(scope, receive, send)
        assert sent[0]["status"] == 503
        assert middleware.active == 1
        release.set()
        try:
            await first
        except RuntimeError:
            pass
        assert middleware.active == 0
    asyncio.run(check())


def test_monitoring_does_not_block_event_loop():
    async def check():
        event_thread = threading.get_ident()
        class Engine:
            def is_healthy(self):
                assert threading.get_ident() != event_thread
                time.sleep(.05)
                return True
            def get_ram_usage_bytes(self): return 1
            def get_disk_usage_bytes(self): return 2
            def list_collections(self): return []
        monitor = MonitoringSocketServer(settings=AdminApiSettings(), state=MonitoringState(), engine_provider=Engine)
        task = asyncio.create_task(monitor.collect_metrics())
        ticks = 0
        while not task.done():
            await asyncio.sleep(.005)
            ticks += 1
        assert (await task)["databaseHealthy"]
        assert ticks >= 3
    asyncio.run(check())


def test_document_page_stream_and_scope():
    class Stream:
        def __init__(self, rows): self.rows = iter(rows); self.closed = False
        def __next__(self): return next(self.rows)
        def close(self): self.closed = True
    class Engine:
        rows = [{"_id": "a"}, {"_id": "b"}]
        def is_internal_app_token_active(self, token): return True
        def find_many(self, collection, **kwargs):
            pytest.fail("HTTP pages must not materialize unbounded FindMany results")
        def iter_documents(self, collection, **kwargs):
            self.stream = Stream(self.rows)
            return self.stream
    engine = Engine()
    app = FastAPI()
    app.state.settings = settings = AdminApiSettings()
    app.include_router(create_api_router(engine_provider=lambda: engine, graph_manager_provider=lambda: None))
    def headers(scopes):
        token = create_app_token(app_id="test", app_name="test", scopes=scopes, secret=settings.api_token_secret)
        return {"Authorization": "Bearer " + token.token}
    with TestClient(app) as client:
        url = "/api/v1/collections/items/documents"
        assert client.get(url, headers=headers(["query:execute"])).status_code == 403
        engine.rows = [{"_id": "a"}, {"_id": "b"}, {"_id": "c"}]
        response = client.get(url + "?limit=1&skip=2", headers=headers(["documents:read"]))
        assert response.json() == {"rows": [{"_id": "c"}], "nextSkip": 3}
        assert engine.stream.closed
        response = client.get(url + "/stream?limit=1", headers=headers(["documents:read"]))
        assert [json.loads(line) for line in response.text.splitlines()] == [{"document": {"_id": "a"}}, {"end": True}]
        assert engine.stream.closed
        engine.rows = [{"_id": str(i)} for i in range(1501)]
        response = client.get(url + "/stream?page_size=100", headers=headers(["documents:read"]))
        records = [json.loads(line) for line in response.text.splitlines()]
        assert len(records) == 1502 and records[-1] == {"end": True}
        assert [record["document"]["_id"] for record in records[:-1]] == [str(i) for i in range(1501)]
        assert engine.stream.closed
        assert client.get(url + "?limit=1001", headers=headers(["documents:read"])).status_code == 422
        engine.rows = [{"_id": str(i), "payload": "x" * 900000} for i in range(6)]
        # Repeated oversized pages must close and return their per-owner slot.
        for _ in range(3):
            response = client.get(url + "?limit=6", headers=headers(["documents:read"]))
            assert response.status_code == 413
            assert engine.stream.closed


def test_admission_releases_on_cancellation():
    async def check():
        entered = asyncio.Event()
        async def application(scope, receive, send):
            entered.set()
            await asyncio.Event().wait()
        middleware = AdmissionMiddleware(application, 1)
        async def receive():
            return {"type": "http.request", "body": b""}
        task = asyncio.create_task(middleware({"type": "http"}, receive, None))
        await entered.wait()
        assert middleware.active == 1
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        assert middleware.active == 0
    asyncio.run(check())


def test_http_select_preserves_user_limit(monkeypatch):
    from nexoradb_admin import query_runner
    class Select:
        limit = 0
    statement = Select()
    observed = []
    class Executor:
        def __init__(self, *args): pass
        def execute_statements(self, statements):
            observed.append(statements[0].limit)
            return []
        def abort_transaction(self): pass
    parser = SimpleNamespace(Executor=Executor, parse=lambda *args: [statement])
    monkeypatch.setattr(query_runner, "_load_nexoraql_package", lambda: parser)
    query_runner.execute_query(engine=None, graph_manager=None,
                              payload=query_runner.QueryExecuteRequest(query="SELECT * FROM items"))
    assert observed == [0]
    for total in (1001, 50000):
        statement.limit = total
        query_runner.execute_query(engine=None, graph_manager=None,
                                  payload=query_runner.QueryExecuteRequest(query="SELECT * FROM items"))
    assert observed == [0, 1001, 50000]
