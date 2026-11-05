"""Real C++/HTTP integration, including direct-binding bypass attempts."""

import importlib.util
import json
import os
import sys
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parents[1]
os.environ.setdefault(
    "NEXORADB_NATIVE_MODULE_PATH",
    str(ROOT / "nexoradb.cpython-310-x86_64-linux-gnu.so"),
)
sys.path.insert(0, str(ROOT / "NexoraDB/src"))
from nexoradb_admin.app import create_app  # noqa: E402
from nexoradb_admin.config import AdminApiSettings  # noqa: E402
from nexoradb_admin.native import load_native_module  # noqa: E402
from nexoradb_admin.native_iam import NativeIamService  # noqa: E402

PASSWORD = "Terminal-secret-test-8931!"


@pytest.mark.parametrize("query", ["SELECT * FROM items;", "SELECT value FROM items;"])
def test_native_stream_preserves_ndjson_document_envelope(iam, query):
    client, service, _native, owner = iam
    engine = service.bind(owner["Authorization"].removeprefix("Bearer "))
    assert engine.create_collection("items").success
    assert engine.insert_one("items", '{"_id":"one","value":7,"end":true}').success
    response = client.post("/query/stream", headers=owner, json={"query": query, "pageSize": 1})
    assert response.status_code == 200, response.text
    frames = [json.loads(line) for line in response.text.splitlines()]
    assert frames[-1] == {"end": True}
    assert len(frames) == 2 and frames[0]["document"]["value"] == 7
    assert not any("error" in frame for frame in frames)


@pytest.fixture
def iam(tmp_path):
    binary = Path(
        os.environ.get("NEXORADB_TEST_IAM_CLI", ROOT / "build/debug/nexora_iam")
    )
    module = Path(os.environ["NEXORADB_NATIVE_MODULE_PATH"])
    if not binary.exists() or not module.exists():
        pytest.skip("Build nexora_iam and nexoradb_py first")
    spec = importlib.util.spec_from_file_location(
        "iam_terminal_test", ROOT / "scripts/verify_iam_cli.py"
    )
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    path = tmp_path / "database"
    helper.terminal_enroll(binary, path)
    settings = AdminApiSettings(
        db_path=path,
        graph_dir=tmp_path / "graph",
        native_module_path=module,
        rocks_auto_tune=False,
        rocks_memory_mb=32,
        rocks_cache_mb=8,
    )
    native = load_native_module(settings)
    service = NativeIamService(
        native, native.SecureDatabase(str(path)), str(settings.graph_dir)
    )
    app = create_app(settings=settings, engine=service)
    with TestClient(app) as client:
        result = client.post(
            "/auth/login", json={"username": "selected.owner", "password": PASSWORD}
        )
        assert result.status_code == 200, result.text
        token = result.json()["accessToken"]
        yield client, service, native, {"Authorization": "Bearer " + token}


def test_native_http_collections_roles_default_deny_and_revocation(iam):
    client, service, native, owner = iam
    assert client.get("/collections").status_code == 401
    assert (
        client.post("/collections", headers=owner, json={"name": "items"}).status_code
        == 201
    )
    created = client.post(
        "/security/users",
        headers=owner,
        json={"username": "reader", "password": PASSWORD},
    )
    assert created.status_code == 201, created.text
    identity = created.json()["_id"]
    login = client.post(
        "/auth/login", json={"username": "reader", "password": PASSWORD}
    ).json()
    reader = {"Authorization": "Bearer " + login["accessToken"]}
    assert client.get("/collections/items/documents", headers=reader).status_code == 403
    assert client.get("/security/users", headers=reader).status_code == 403
    assert (
        client.patch(
            "/security/users/" + identity,
            headers=owner,
            json={"grants": {"items": ["read"]}},
        ).status_code
        == 200
    )
    assert client.get("/auth/me", headers=reader).status_code == 401
    _assert_reader_access_then_disable(client, service, native, owner, identity)


def _assert_reader_access_then_disable(client, service, native, owner, identity):
    token = client.post(
        "/auth/login", json={"username": "reader", "password": PASSWORD}
    ).json()["accessToken"]
    reader = {"Authorization": "Bearer " + token}
    assert client.get("/collections/items/documents", headers=reader).status_code == 200
    assert client.get("/graphs", headers=reader).status_code == 403
    assert client.get("/system/execution", headers=reader).status_code == 403
    request = native.SecureDataRequest()
    request.operation, request.collection = "insert_one", "items"
    request.payload = '{"_id":"denied"}'
    with pytest.raises(native.AuthorizationError):
        service.gateway.execute(token, request)
    assert not hasattr(service.gateway, "engine")
    assert (
        client.patch(
            "/security/users/" + identity, headers=owner, json={"status": "disabled"}
        ).status_code
        == 200
    )
    assert client.get("/auth/me", headers=reader).status_code == 401


def test_only_initial_owner_can_create_or_promote_superusers(iam):
    client, service, native, owner = iam
    created = client.post(
        "/security/users",
        headers=owner,
        json={
            "username": "delegated",
            "password": PASSWORD,
            "roles": ["superuser"],
            "grants": {"posts": ["read"]},
        },
    )
    assert created.status_code == 201, created.text
    assert created.json()["roles"] == ["superuser"]
    assert created.json()["grants"] == {"posts": ["read"]}
    login = client.post(
        "/auth/login", json={"username": "delegated", "password": PASSWORD}
    )
    delegated = {"Authorization": "Bearer " + login.json()["accessToken"]}
    assert not login.json()["user"]["protectedRoot"]
    denied = client.post(
        "/security/users",
        headers=delegated,
        json={
            "username": "escalated",
            "password": PASSWORD,
            "roles": ["superuser"],
        },
    )
    assert denied.status_code == 403, denied.text
    users = client.get("/security/users", headers=owner).json()
    assert not any(user["username"] == "escalated" for user in users)
    ordinary = client.post(
        "/security/users",
        headers=delegated,
        json={
            "username": "ordinary",
            "password": PASSWORD,
            "grants": {"posts": ["read"]},
        },
    )
    assert ordinary.status_code == 201, ordinary.text
    identity = ordinary.json()["_id"]
    assert (
        client.patch(
            "/security/users/" + identity,
            headers=delegated,
            json={"roles": ["superuser"]},
        ).status_code
        == 403
    )
    root = next(user for user in users if user["protectedRoot"])
    assert (
        client.post(
            "/security/users/" + root["_id"] + "/reset", headers=delegated
        ).status_code
        == 403
    )
    ordinary_login = client.post(
        "/auth/login", json={"username": "ordinary", "password": PASSWORD}
    ).json()
    assert (
        client.post(
            "/security/users",
            headers={"Authorization": "Bearer " + ordinary_login["accessToken"]},
            json={
                "username": "forbidden",
                "password": PASSWORD,
            },
        ).status_code
        == 403
    )
    # Native binding enforces the same creation rule, independently of HTTP/UI.
    with pytest.raises(native.AuthorizationError):
        service.gateway.administer(
            login.json()["accessToken"],
            "create_user",
            json.dumps(
                {
                    "username": "native_escalation",
                    "password": PASSWORD,
                    "roles": ["superuser"],
                }
            ),
        )


def test_security_nexoraql_role_definitions_assignment_and_escalation(iam):
    import uuid

    client, _service, _native, owner = iam

    def query(text, headers=owner):
        return client.post(
            "/query/execute",
            headers={**headers, "Idempotency-Key": str(uuid.uuid4())},
            json={"query": text},
        )

    response = query(
        'CREATE OR REPLACE ROLE social_reader GRANTS {"posts":["read"]}; CREATE OR REPLACE ROLE social_editor GRANTS {"posts":["write"]} INHERITS ["social_reader"];'
    )
    assert response.status_code == 200, response.text
    definitions = client.get("/security/roles", headers=owner).json()
    assert definitions["social_editor"]["parents"] == ["social_reader"]
    user = client.post(
        "/security/users",
        headers=owner,
        json={"username": "editor", "password": PASSWORD},
    ).json()
    result = query(
        f'ALTER USER \'{user["_id"]}\' SET ROLES ["social_editor"]; ALTER USER \'{user["_id"]}\' SET GRANTS {{"follows":["read"]}};'
    )
    assert result.status_code == 200, result.text
    current = next(
        item
        for item in client.get("/security/users", headers=owner).json()
        if item["_id"] == user["_id"]
    )
    assert current["roles"] == ["social_editor"] and current["grants"] == {
        "follows": ["read"]
    }
    assert query("SHOW SECURITY USERS; SHOW SECURITY ROLES;").status_code == 200
    tx = query("BEGIN TRANSACTION; CREATE OR REPLACE ROLE forbidden GRANTS {}; COMMIT;")
    assert tx.status_code == 400, tx.text
    assert "forbidden" not in client.get("/security/roles", headers=owner).json()
    delegated = client.post(
        "/security/users",
        headers=owner,
        json={"username": "second", "password": PASSWORD, "roles": ["superuser"]},
    ).json()
    token = client.post(
        "/auth/login", json={"username": "second", "password": PASSWORD}
    ).json()["accessToken"]
    denied = query(
        f"ALTER USER '{user['_id']}' SET ROLES [\"superuser\"];",
        {"Authorization": "Bearer " + token},
    )
    assert denied.status_code == 403, denied.text
    assert delegated["protectedRoot"] is False
    ordinary = client.post(
        "/auth/login", json={"username": "editor", "password": PASSWORD}
    ).json()
    assert (
        query(
            "SHOW SECURITY USERS;",
            {"Authorization": "Bearer " + ordinary["accessToken"]},
        ).status_code
        == 403
    )
    cycle = query(
        'CREATE OR REPLACE ROLE social_reader GRANTS {} INHERITS ["social_editor"];'
    )
    assert cycle.status_code == 422, cycle.text
    assert (
        client.get("/security/roles", headers=owner).json()["social_reader"]["parents"]
        == []
    )


def test_native_reset_replay_password_change_logout_and_validation_redaction(iam):
    client, service, native, owner = iam
    created = client.post(
        "/security/users",
        headers=owner,
        json={"username": "recoverable", "password": PASSWORD},
    ).json()
    response = client.post(f"/security/users/{created['_id']}/reset", headers=owner)
    assert response.status_code == 200, response.text
    factor = response.json()["resetToken"]
    reset = {"resetToken": factor, "newPassword": "replacement password with spaces "}
    assert client.post("/auth/reset-password", json=reset).status_code == 200
    assert client.post("/auth/reset-password", json=reset).status_code == 401
    login = client.post(
        "/auth/login",
        json={"username": "recoverable", "password": reset["newPassword"]},
    )
    assert login.status_code == 200, login.text
    headers = {"Authorization": "Bearer " + login.json()["accessToken"]}
    assert (
        client.post(
            "/auth/change-password",
            headers=headers,
            json={
                "currentPassword": reset["newPassword"],
                "newPassword": "next replacement password",
            },
        ).status_code
        == 200
    )
    assert client.get("/auth/me", headers=headers).status_code == 401
    secret = "sensitive-invalid-input"
    bad = client.post(
        "/auth/reset-password", json={"resetToken": secret, "newPassword": secret}
    )
    assert bad.status_code == 422 and secret not in bad.text
    assert client.post("/auth/logout", headers=owner).status_code == 204
    assert client.get("/auth/me", headers=owner).status_code == 401


def test_native_service_token_query_and_revoke(iam):
    client, service, native, owner = iam
    response = client.post(
        "/query/execute", headers=owner, json={"query": "CREATE COLLECTION items;"}
    )
    assert response.status_code == 200, response.text
    issued = client.post(
        "/apps/tokens",
        headers=owner,
        json={
            "appId": "test.app",
            "appName": "Read only",
            "scopes": ["query:execute", "documents:read", "collections:read"],
            "collections": {"items": ["read"]},
            "expiresInSeconds": 300,
        },
    )
    assert issued.status_code == 200, issued.text
    headers = {"Authorization": "Bearer " + issued.json()["token"]}
    for statement in (
        "SHOW SECURITY USERS;",
        "CREATE OR REPLACE ROLE forbidden GRANTS {};",
    ):
        assert (
            client.post(
                "/api/v1/query", headers=headers, json={"query": statement}
            ).status_code
            == 403
        )
    assert client.get("/auth/me", headers=headers).status_code == 403
    result = client.post(
        "/api/v1/query", headers=headers, json={"query": "SELECT * FROM items;"}
    )
    assert result.status_code == 200, result.text
    assert (
        client.delete(
            "/apps/tokens/" + issued.json()["tokenId"], headers=owner
        ).status_code
        == 204
    )
    assert (
        client.post(
            "/api/v1/query", headers=headers, json={"query": "SELECT * FROM items;"}
        ).status_code
        == 401
    )


def test_native_query_stream_graph_and_audit(iam):
    client, service, native, owner = iam
    result = client.post(
        "/query/execute", headers=owner, json={"query": "CREATE COLLECTION items;"}
    )
    assert result.status_code == 200, result.text
    assert client.get("/graphs", headers=owner).status_code == 200
    for value in ("a", "b", "c"):
        request = native.SecureDataRequest()
        request.operation, request.collection = "insert_one", "items"
        request.payload = json.dumps({"_id": value})
        assert service.gateway.execute(owner["Authorization"][7:], request).success
    result = client.post(
        "/query/execute", headers=owner, json={"query": "SELECT * FROM items;"}
    )
    assert result.status_code == 200, result.text
    assert len(result.json()["rows"]) == 3
    audit = client.get("/security/audit", headers=owner)
    assert audit.status_code == 200 and PASSWORD not in audit.text


def test_real_driver_native_login_transactions_and_logout(iam, monkeypatch):
    client, service, native, owner = iam
    import httpx
    from nexoradb.api import driver as module

    original = httpx.Client

    def transport(request):
        response = client.request(
            request.method,
            str(request.url),
            content=request.content,
            headers=dict(request.headers),
        )
        if request.url.path == "/api/v1/query":
            assert response.status_code == 200, response.text
        return httpx.Response(
            response.status_code,
            headers=dict(response.headers),
            content=response.content,
        )

    mock = httpx.MockTransport(transport)
    monkeypatch.setattr(
        module.httpx, "Client", lambda **kwargs: original(transport=mock, **kwargs)
    )
    connection = module.NexoraDBClient.connect(
        url="https://database.example", username="selected.owner", password=PASSWORD
    )
    token = connection.token
    connection.execute("CREATE COLLECTION transactional;")
    result = connection.execute(
        'BEGIN TRANSACTION; INSERT INTO transactional VALUES ({"_id":"one"}); COMMIT;'
    )
    assert not result.raw.get("statementErrors")
    found = connection.execute("SELECT * FROM transactional;")
    assert found.rows[0]["_id"] == "one"
    connection.logout()
    assert (
        client.get("/auth/me", headers={"Authorization": "Bearer " + token}).status_code
        == 401
    )
    with module.NexoraDBClient.connect(
        url="https://database.example", username="selected.owner", password=PASSWORD
    ) as temporary:
        temporary_token = temporary.token
    assert (
        client.get(
            "/auth/me", headers={"Authorization": "Bearer " + temporary_token}
        ).status_code
        == 401
    )


def test_reauthentication_cannot_replay_old_data_or_repeat_a_keyed_write(iam):
    client, service, native, owner = iam
    assert (
        client.post("/collections", headers=owner, json={"name": "items"}).status_code
        == 201
    )
    body = {"query": 'INSERT INTO items VALUES ({"_id":"once"});'}
    headers = {**owner, "Idempotency-Key": "credential-bound-write"}
    assert client.post("/api/v1/query", headers=headers, json=body).status_code == 200
    login = client.post(
        "/auth/login", json={"username": "selected.owner", "password": PASSWORD}
    ).json()
    replacement = {
        "Authorization": "Bearer " + login["accessToken"],
        "Idempotency-Key": "credential-bound-write",
    }
    assert (
        client.post("/api/v1/query", headers=replacement, json=body).status_code == 409
    )


def test_buffered_stream_rows_do_not_survive_permission_revocation(iam):
    client, service, native, owner = iam
    assert (
        client.post("/collections", headers=owner, json={"name": "items"}).status_code
        == 201
    )
    for index in range(3):
        request = native.SecureDataRequest()
        request.operation, request.collection = "insert_one", "items"
        request.payload = json.dumps({"_id": str(index)})
        assert service.gateway.execute(owner["Authorization"][7:], request).success
    user = client.post(
        "/security/users",
        headers=owner,
        json={"username": "reader", "password": PASSWORD},
    ).json()
    assert (
        client.patch(
            "/security/users/" + user["_id"],
            headers=owner,
            json={"grants": {"items": ["read"]}},
        ).status_code
        == 200
    )
    login = client.post(
        "/auth/login", json={"username": "reader", "password": PASSWORD}
    ).json()
    stream = service.bind(login["accessToken"]).iter_documents("items", page_size=3)
    assert next(stream)["_id"] == "0"
    assert (
        client.patch(
            "/security/users/" + user["_id"], headers=owner, json={"grants": {}}
        ).status_code
        == 200
    )
    from fastapi import HTTPException

    with pytest.raises(HTTPException) as denied:
        next(stream)
    assert denied.value.status_code == 401
    stream.close()


def test_native_projection_refresh_does_not_require_exposing_graph_access(iam):
    client, service, native, owner = iam
    assert (
        client.post("/collections", headers=owner, json={"name": "items"}).status_code
        == 201
    )
    definition = native.GraphDefinition()
    definition.name = "mapped"
    mapping = native.NodeMappingDef()
    mapping.node_type, mapping.collection, mapping.key_path = "Item", "items", "_id"
    definition.node_mappings.append(mapping)
    token = owner["Authorization"][7:]
    assert service.graph.gateway.call(token, "create_graph", [definition])
    user = client.post(
        "/security/users",
        headers=owner,
        json={"username": "writer", "password": PASSWORD},
    ).json()
    assert (
        client.patch(
            "/security/users/" + user["_id"],
            headers=owner,
            json={"grants": {"items": ["write"]}},
        ).status_code
        == 200
    )
    login = client.post(
        "/auth/login", json={"username": "writer", "password": PASSWORD}
    ).json()
    request = native.SecureDataRequest()
    request.operation, request.collection, request.payload = (
        "insert_one",
        "items",
        '{"_id":"projected"}',
    )
    assert service.gateway.execute(login["accessToken"], request).success
    assert service.graph.gateway.call(token, "get_stats", ["mapped"]).active_nodes == 1
    with pytest.raises(native.AuthorizationError):
        service.graph.gateway.call(login["accessToken"], "get_stats", ["mapped"])


def test_production_factory_requires_offline_enrollment_before_serving(tmp_path):
    from fastapi import HTTPException

    settings = AdminApiSettings(
        db_path=tmp_path / "unenrolled",
        graph_dir=tmp_path / "graph",
        native_module_path=ROOT / "nexoradb.cpython-310-x86_64-linux-gnu.so",
        rocks_auto_tune=False,
        rocks_memory_mb=32,
        rocks_cache_mb=8,
    )
    with pytest.raises(HTTPException) as denied:
        with TestClient(create_app(settings=settings)):
            pytest.fail("An unenrolled native store was served")
    assert denied.value.status_code == 503
