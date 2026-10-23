import asyncio
import importlib
import json
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "NexoraDB/src"))

import httpx
import pytest
from fastapi import FastAPI, HTTPException
from fastapi.testclient import TestClient
from nexoradb.api.app import create_api_router
from nexoradb.api.driver import NexoraDBClient, NexoraDBError, NexoraDBPoolBusy
from nexoradb.api.security import create_app_token, decode_app_token
from nexoradb.api.security import CreateAppTokenRequest
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.cursor import CursorBudget, stream_cursor
from nexoradb_admin.query_runner import _load_nexoraql_package, authorize_collections


def test_raw_cursor_limits_auth_checks_and_cleanup():
    async def check():
        calls, authorized, output = [], [], []
        class Raw:
            closed = False
            def next_ndjson_batch(self, limit):
                calls.append(limit)
                return b'{"document":{"_id":"x"}}\n' * limit, limit
            def __next__(self):
                pytest.fail("raw path must not decode Python documents")
            def close(self): self.closed = True
        stream, budget = Raw(), CursorBudget()
        response = await stream_cursor(lambda: stream, lambda: authorized.append(1),
                                       "a", budget, batch_size=2, limit=3)
        async for chunk in response.body_iterator:
            output.append(chunk)
        assert calls == [2, 1]
        assert len(authorized) == 3
        assert output[-1] == '{"end":true}\n'
        assert stream.closed and budget.owners == {}
    asyncio.run(check())


def test_native_raw_http_and_select_cursor(tmp_path):
    from nexoradb_admin.native import load_native_module
    native_path = Path(__file__).resolve().parents[1] / 'nexoradb.cpython-310-x86_64-linux-gnu.so'
    if not native_path.exists():
        pytest.skip('Linux Release extension not built')
    native = load_native_module(AdminApiSettings(native_module_path=native_path))
    engine = native.DocEngine(str(tmp_path / 'db'))
    assert engine.create_collection('items').success
    expected = [{'_id': str(i), 'text': 'فارسی },{ \\"\n', 'nested': [i, {'x': '}'}]}
                for i in range(3)]
    assert engine.insert_many('items', [json.dumps(row) for row in expected]).success
    app = FastAPI()
    app.state.settings = settings = AdminApiSettings()
    app.include_router(create_api_router(engine_provider=lambda: engine, graph_manager_provider=lambda: None))
    token = create_app_token(app_id='raw', app_name='raw', scopes=['documents:read', 'query:execute'],
                             secret=settings.api_token_secret)
    assert engine.create_internal_app_token(token.tokenId, json.dumps({'appId': 'raw'})).success
    headers = {'Authorization': 'Bearer ' + token.token}
    with TestClient(app) as client:
        for response in (
            client.get('/api/v1/collections/items/documents/stream?limit=2&page_size=1', headers=headers),
            client.post('/api/v1/query/cursor?batch_size=1', headers=headers,
                        json={'query': 'SELECT * FROM items LIMIT 2;'}),
        ):
            assert response.status_code == 200, response.text
            assert [json.loads(line) for line in response.text.splitlines()] == [
                {'document': row} for row in expected[:2]] + [{'end': True}]
    assert engine.prepare_shutdown().success


def test_collection_grants_cover_join_and_entire_request():
    parser = _load_nexoraql_package()
    grants = {"allowed": ["read"]}
    authorize_collections(parser.parse("SELECT * FROM allowed;"), grants)
    for query in ["SELECT * FROM secret;", "DELETE FROM allowed WHERE _id = 'one';", "SHOW COLLECTIONS;",
                  "SELECT * FROM allowed; SELECT * FROM secret;"]:
        with pytest.raises(HTTPException) as denied:
            authorize_collections(parser.parse(query), grants)
        assert denied.value.status_code == 403
    statement = SimpleNamespace(collection="allowed", joins=[("secret", "id", "id")])
    Select = type("Select", (), {})
    join = Select()
    join.__dict__.update(statement.__dict__)
    with pytest.raises(HTTPException):
        authorize_collections([join], grants)


def test_signed_grants_roundtrip():
    secret = "s" * 40
    token = create_app_token(app_id="app", app_name="app", scopes=["documents:read"],
                             secret=secret, collections={"allowed": ["read"]})
    assert decode_app_token(token.token, secret).collections == {"allowed": ["read"]}


def test_new_grants_default_deny_and_reject_reserved_or_wildcard_names():
    assert CreateAppTokenRequest(appId="app").collections == {}
    for name in ("*", "__nexora_internal_users", "orders:other"):
        with pytest.raises(ValueError):
            CreateAppTokenRequest(appId="app", collections={name: ["read"]})


def test_production_rejects_legacy_broad_application_tokens():
    settings = AdminApiSettings(environment="production")
    token = create_app_token(app_id="app", app_name="app", scopes=["documents:read"], secret=settings.api_token_secret)
    app = FastAPI()
    app.state.settings = settings
    app.include_router(create_api_router(engine_provider=lambda: pytest.fail("legacy token reached engine"), graph_manager_provider=lambda: None))
    with TestClient(app, base_url="https://testserver") as client:
        response = client.get("/api/v1/collections/allowed/documents", headers={"Authorization": "Bearer " + token.token})
        assert response.status_code == 403


def test_query_cursor_projection_limit_skip_acl_and_rejection(monkeypatch):
    parser = _load_nexoraql_package()
    semantic = importlib.import_module(parser.__name__ + ".semantic")
    monkeypatch.setattr(semantic, "build_condition", lambda condition: condition)
    class Stream:
        def __init__(self): self.rows = iter([{"_id": str(i), "value": i, "private": "x"} for i in range(8)]); self.closed = False
        def __next__(self): return next(self.rows)
        def close(self): self.closed = True
    class Engine:
        active = True
        def is_internal_app_token_active(self, token): return self.active
        def iter_documents(self, collection, condition, **kwargs):
            assert collection == "allowed"
            self.stream = Stream()
            return self.stream
    engine = Engine()
    settings = AdminApiSettings()
    token = create_app_token(app_id="app", app_name="app", scopes=["query:execute", "documents:read"],
                             secret=settings.api_token_secret, collections={"allowed": ["read"]})
    app = FastAPI()
    app.state.settings = settings
    app.include_router(create_api_router(engine_provider=lambda: engine, graph_manager_provider=lambda: None))
    headers = {"Authorization": "Bearer " + token.token}
    with TestClient(app) as client:
        response = client.post("/api/v1/query/cursor?batch_size=2",
                               json={"query": "SELECT value FROM allowed LIMIT 3 SKIP 2;"}, headers=headers)
        assert response.status_code == 200, response.text
        records = [json.loads(line) for line in response.text.splitlines()]
        assert records == [{"document": {"_id": str(i), "value": i}} for i in (2, 3, 4)] + [{"end": True}]
        assert engine.stream.closed
        assert client.post("/api/v1/query/cursor", json={"query": "SELECT * FROM secret;"}, headers=headers).status_code == 403
        assert client.post("/api/v1/query/cursor", json={"query": "DELETE FROM allowed WHERE _id = 'one';"}, headers=headers).status_code == 422
        assert client.get("/api/v1/collections/secret/documents", headers=headers).status_code == 403


def test_cursor_budget_and_failure_cleanup():
    async def check():
        budget = CursorBudget(total=2, per_owner=1)
        budget.acquire("app")
        with pytest.raises(HTTPException) as busy:
            budget.acquire("app")
        assert busy.value.status_code == 429
        budget.release("app")
        class Stream:
            closed = False
            def __next__(self): raise RuntimeError("corruption")
            def close(self): self.closed = True
        stream = Stream()
        with pytest.raises(RuntimeError):
            await stream_cursor(lambda: stream, lambda: None, "app", budget)
        assert stream.closed and budget.owners == {}
    asyncio.run(check())


def test_cursor_revocation_between_batches_and_deadline_cleanup():
    async def check(slow):
        class Stream:
            closed = False
            def __next__(self): return {"value": 1}
            def close(self): self.closed = True
        stream, budget = Stream(), CursorBudget()
        revoked = False
        def authorize():
            if revoked:
                raise HTTPException(401, "revoked")
        response = await stream_cursor(lambda: stream, authorize, "app", budget,
                                       batch_size=1, max_seconds=.05 if slow else 1)
        sent = []
        async def send(message):
            nonlocal revoked
            if message["type"] == "http.response.body":
                sent.append(message.get("body", b""))
                revoked = True
                if slow:
                    await asyncio.sleep(.2)
        async def receive(): return {"type": "http.disconnect"}
        scope = {"type": "http", "method": "GET", "asgi": {"spec_version": "2.4"}}
        if slow:
            with pytest.raises(TimeoutError):
                await response(scope, receive, send)
        else:
            await response(scope, receive, send)
            assert b'"error"' in b"".join(sent) and b'"end":true' not in b"".join(sent)
        assert stream.closed and budget.owners == {}
    asyncio.run(check(False))
    asyncio.run(check(True))


def test_cursor_batches_are_byte_bounded_for_large_documents():
    async def check():
        class Stream:
            closed = False
            def __next__(self): return {"payload": "x" * 800000}
            def close(self): self.closed = True
        stream, budget = Stream(), CursorBudget()
        response = await stream_cursor(lambda: stream, lambda: None, "app", budget,
                                       batch_size=1000, limit=4)
        sizes = []
        async def send(message):
            if message["type"] == "http.response.body":
                sizes.append(len(message.get("body", b"")))
        async def receive(): return {"type": "http.disconnect"}
        await response({"type": "http", "method": "GET", "asgi": {"spec_version": "2.4"}}, receive, send)
        assert max(sizes) < 3 * 1024 * 1024
        assert sum(size > 100000 for size in sizes) == 2
        assert stream.closed and budget.owners == {}
    asyncio.run(check())


def test_driver_lazy_cursor_closes_transport_on_early_stop():
    class Body(httpx.SyncByteStream):
        closed = False
        def __iter__(self):
            yield b'{"document":{"id":1}}\n'
            yield b'{"document":{"id":2}}\n{"end":true}\n'
        def close(self): self.closed = True
    body = Body()
    with NexoraDBClient(url="http://localhost", token="test") as client:
        client._client.close()
        client._client = httpx.Client(transport=httpx.MockTransport(lambda request: httpx.Response(200, stream=body)))
        with client.cursor("SELECT * FROM items;") as rows:
            assert next(rows) == {"id": 1}
        assert body.closed


def test_driver_incomplete_cursor_is_not_success():
    with NexoraDBClient(url="http://localhost", token="test") as client:
        client._client.close()
        client._client = httpx.Client(transport=httpx.MockTransport(lambda request: httpx.Response(200, content=b'{"document":{}}\n')))
        with pytest.raises(NexoraDBError, match="before completion"):
            with client.cursor("SELECT * FROM items;") as rows:
                list(rows)


def test_pool_acquisition_timeout_before_transport():
    with NexoraDBClient(url="http://localhost", token="test", max_connections=1, pool_timeout=.01) as client:
        with client._lease():
            with pytest.raises(NexoraDBPoolBusy):
                client.ping()
        assert client.pool_stats()["active"] == 0
        assert client.pool_stats()["acquisition_timeouts"] == 1


def test_pool_refuses_insecure_tls_context_and_inherited_process(monkeypatch):
    import ssl
    import os
    unsafe = ssl._create_unverified_context()
    with pytest.raises(ValueError, match="verification"):
        NexoraDBClient(url="https://example.test", token="secret", ssl_context=unsafe)
    with NexoraDBClient(url="http://localhost", token="test") as client:
        monkeypatch.setattr(os, "getpid", lambda: client._pid + 1)
        with pytest.raises(NexoraDBError, match="after fork"):
            client.ping()


def test_pooled_client_does_not_follow_redirect():
    seen = []
    def handler(request):
        seen.append(str(request.url))
        return httpx.Response(302, headers={"Location": "https://evil.test/steal"})
    with NexoraDBClient(url="https://database.test", token="secret") as client:
        client._client.close()
        client._client = httpx.Client(transport=httpx.MockTransport(handler), follow_redirects=False)
        with pytest.raises(NexoraDBError, match="302"):
            client.ping()
    assert seen == ["https://database.test/api/v1/health"]


def test_real_http_keepalive_reuses_connection_and_closes():
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    peers = []
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def do_GET(self):
            peers.append(self.client_address)
            body = b'{"message":"ok"}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *args): pass
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with NexoraDBClient(url=f"http://127.0.0.1:{server.server_port}", token="test") as client:
            assert all(client.ping() for _ in range(3))
        assert len(peers) == 3 and len(set(peers)) == 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join(2)
