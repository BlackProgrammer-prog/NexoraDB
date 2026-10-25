import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "NexoraDB/src"))

import pytest
from fastapi import HTTPException
from nexoradb_admin.idempotency import RequestJournal
from nexoradb_admin.admission import AdmissionMiddleware
from nexoradb.api.driver import NexoraDBClient, NexoraDBOutcomeUnknown


def test_journal_replay_restart_conflict_and_principal(tmp_path):
    path = tmp_path / "requests.sqlite3"
    journal = RequestJournal(path)
    calls = []
    def action():
        calls.append(1)
        return {"rows": [{"value": 1}]}
    expected = journal.run("alice", "key-1", {"a": 1}, action)
    assert RequestJournal(path).run("alice", "key-1", {"a": 1}, action) == expected
    assert calls == [1]
    with pytest.raises(HTTPException) as error:
        journal.run("alice", "key-1", {"a": 2}, action)
    assert error.value.status_code == 409
    journal.run("bob", "key-1", {"a": 1}, action)
    assert calls == [1, 1]


def test_concurrent_duplicate_does_not_execute_twice(tmp_path):
    journal = RequestJournal(tmp_path / "requests.sqlite3")
    entered, release = threading.Event(), threading.Event()
    def action():
        entered.set()
        assert release.wait(5)
        return {"success": True}
    with ThreadPoolExecutor(2) as pool:
        first = pool.submit(journal.run, "a", "key", {}, action)
        assert entered.wait(5)
        try:
            with pytest.raises(HTTPException) as error:
                journal.run("a", "key", {}, lambda: pytest.fail("duplicate execution"))
            assert error.value.status_code == 409
        finally:
            release.set()
        assert first.result() == {"success": True}


def test_crash_reservation_never_reexecutes(tmp_path):
    import os
    import subprocess
    path = tmp_path / "requests.sqlite3"
    source = "from nexoradb_admin.idempotency import RequestJournal; import os,sys; RequestJournal(sys.argv[1]).run('a','key',{},lambda:os._exit(17))"
    environment = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[1] / "NexoraDB/src"))
    process = subprocess.run([sys.executable, "-c", source, str(path)], env=environment, cwd=tmp_path, timeout=10)
    assert process.returncode == 17
    with pytest.raises(HTTPException) as error:
        RequestJournal(path).run("a", "key", {}, lambda: pytest.fail("uncertain write repeated"))
    assert error.value.status_code == 409


def test_chunked_request_limit_precedes_endpoint():
    async def check():
        async def app(*args): pytest.fail("oversize body reached endpoint")
        middleware = AdmissionMiddleware(app, max_body_bytes=3)
        messages = iter([{"type": "http.request", "body": b"ab", "more_body": True},
                         {"type": "http.request", "body": b"cd", "more_body": False}])
        async def receive(): return next(messages)
        sent = []
        async def send(message): sent.append(message)
        await middleware({"type": "http"}, receive, send)
        assert sent[0]["status"] == 413
        assert middleware.active == 0
    asyncio.run(check())


def test_fragmented_body_is_replayed_exactly_once():
    async def check():
        fragments = iter([b"", b"a", b"", b"b", b"", b"c"])
        async def receive():
            fragment = next(fragments)
            return {"type": "http.request", "body": fragment, "more_body": fragment != b"c"}
        async def app(scope, receive, send):
            assert await receive() == {"type": "http.request", "body": b"abc", "more_body": False}
        middleware = AdmissionMiddleware(app, max_body_bytes=3)
        await middleware({"type": "http"}, receive, None)
        assert middleware.active == 0
    asyncio.run(check())


def test_driver_tls_redirect_timeout_and_key(monkeypatch):
    with pytest.raises(ValueError, match="HTTPS"):
        NexoraDBClient(url="http://remote.example", token="secret")
    client = NexoraDBClient(url="https://remote.example", token="secret")
    calls = []
    def fail(method, url, **kwargs):
        calls.append(kwargs)
        raise TimeoutError()
    monkeypatch.setattr(client._client, "stream", fail)
    with pytest.raises(NexoraDBOutcomeUnknown):
        client.execute("UPDATE items SET value=2;", idempotency_key="operation-1")
    assert len(calls) == 1
    assert calls[0]["headers"]["Idempotency-Key"] == "operation-1"
    assert "_idempotency_key" not in json.loads(calls[0]["content"])
    client.close()


def test_http_replay_checks_auth_and_durability(tmp_path, monkeypatch):
    import importlib
    from types import SimpleNamespace
    from fastapi import FastAPI
    from fastapi.testclient import TestClient
    from nexoradb.api.security import create_app_token
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.query_runner import QueryExecuteResponse
    api = importlib.import_module("nexoradb.api.app")
    settings = AdminApiSettings(db_path=tmp_path)
    class Engine:
        active = True
        flushed = 0
        def is_internal_app_token_active(self, token): return self.active
        def flush_storage(self):
            self.flushed += 1
            return SimpleNamespace(success=True)
    engine = Engine()
    calls = []
    def execute(**kwargs):
        calls.append(1)
        return QueryExecuteResponse(columns=[], rows=[], raw=None, executionTimeMs=0, statementCount=1)
    monkeypatch.setattr(api, "execute_query", execute)
    app = FastAPI()
    app.state.settings = settings
    app.include_router(api.create_api_router(engine_provider=lambda: engine, graph_manager_provider=lambda: None))
    token = create_app_token(app_id="client", app_name="client", scopes=["query:execute"], secret=settings.api_token_secret)
    headers = {"Authorization": "Bearer " + token.token, "Idempotency-Key": "one-operation"}
    with TestClient(app) as client:
        first = client.post("/api/v1/query", json={"query": "COUNT FROM items;"}, headers=headers)
        assert first.status_code == 200
        assert client.post("/api/v1/query", json={"query": "COUNT FROM items;"}, headers=headers).json() == first.json()
        assert calls == [1] and engine.flushed == 1
        assert client.post("/api/v1/query", json={"query": "SHOW COLLECTIONS;"}, headers=headers).status_code == 409
        engine.active = False
        assert client.post("/api/v1/query", json={"query": "COUNT FROM items;"}, headers=headers).status_code == 401


def test_production_transport_rejects_http_before_endpoint():
    async def check():
        async def app(*args): pytest.fail("insecure request reached endpoint")
        sent = []
        async def send(message): sent.append(message)
        await AdmissionMiddleware(app, require_https=True)({"type": "http", "scheme": "http"}, None, send)
        assert sent[0]["status"] == 400
    asyncio.run(check())
