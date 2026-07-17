from __future__ import annotations

import asyncio
import json
import sys
from dataclasses import dataclass
from pathlib import Path

from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "NexoraDB" / "src"
sys.path.insert(0, str(SRC))

from nexoradb_admin.app import create_app  # noqa: E402
from nexoradb_admin.config import AdminApiSettings  # noqa: E402
from nexoradb_admin.graph_store import GraphMetadataStore  # noqa: E402
from nexoradb_admin.monitoring import MonitoringSocketServer, MonitoringState  # noqa: E402


@dataclass
class FakeResult:
    success: bool
    data: str = ""
    error_msg: str = ""


class FakeInternalUserEngine:
    def __init__(self) -> None:
        self.users: dict[str, dict] = {}
        self.app_tokens: dict[str, dict] = {}
        self.collections: dict[str, dict[str, dict]] = {}

    def create_internal_user(self, user_json: str) -> FakeResult:
        user = json.loads(user_json)
        username = user["username"]
        if username in self.users:
            return FakeResult(False, error_msg="internal user already exists")
        self.users[username] = user
        return FakeResult(True, username)

    def get_internal_user(self, username: str) -> FakeResult:
        if username not in self.users:
            return FakeResult(False, error_msg="internal user not found")
        return FakeResult(True, json.dumps(self.users[username]))

    def update_internal_user(self, username: str, user_json: str) -> FakeResult:
        if username not in self.users:
            return FakeResult(False, error_msg="internal user not found")
        user = json.loads(user_json)
        if user["username"] != username:
            return FakeResult(False, error_msg="username cannot be changed")
        self.users[username] = user
        return FakeResult(True, "1")

    def create_internal_app_token(self, token_id: str, token_json: str) -> FakeResult:
        if token_id in self.app_tokens:
            return FakeResult(False, error_msg="application token already exists")
        self.app_tokens[token_id] = json.loads(token_json)
        return FakeResult(True, token_id)

    def list_internal_app_tokens(self) -> FakeResult:
        return FakeResult(True, json.dumps(list(self.app_tokens.values())))

    def delete_internal_app_token(self, token_id: str) -> FakeResult:
        if token_id not in self.app_tokens:
            return FakeResult(False, error_msg="application token not found")
        self.app_tokens.pop(token_id)
        return FakeResult(True, "1")

    def is_internal_app_token_active(self, token_id: str) -> bool:
        return token_id in self.app_tokens


class FakeGraphMode:
    Live = "live"


class FakeGraphDefinition:
    def __init__(self) -> None:
        self.name = ""
        self.mode = None
        self.directed = True
        self.heterogeneous = True
        self.auto_build_on_startup = False


class FakeNativeModule:
    GraphMode = FakeGraphMode
    GraphDefinition = FakeGraphDefinition


class FakeGraphStats:
    active_nodes = 0
    active_edges = 0
    version = 1


class FakeGraphManager:
    def __init__(self) -> None:
        self.graphs: set[str] = set()

    def create_graph(self, definition: FakeGraphDefinition) -> bool:
        if not definition.name or definition.name in self.graphs:
            return False
        self.graphs.add(definition.name)
        return True

    def drop_graph(self, graph_name: str) -> bool:
        if graph_name not in self.graphs:
            return False
        self.graphs.remove(graph_name)
        return True

    def list_graphs(self) -> list[str]:
        return sorted(self.graphs)

    def get_stats(self, graph_name: str) -> FakeGraphStats:
        if graph_name not in self.graphs:
            raise RuntimeError("graph not found")
        return FakeGraphStats()

    def is_healthy(self) -> bool:
        return True

    def get_ram_usage_bytes(self) -> int:
        return 123_456

    def get_disk_usage_bytes(self) -> int:
        return 654_321

    def create_collection(self, collection: str) -> FakeResult:
        if collection in self.collections:
            return FakeResult(False, error_msg=f"Collection '{collection}' already exists")
        self.collections[collection] = {}
        return FakeResult(True, f"Collection '{collection}' created")

    def drop_collection(self, collection: str) -> FakeResult:
        if collection not in self.collections:
            return FakeResult(False, error_msg=f"Collection '{collection}' does not exist")
        self.collections.pop(collection)
        return FakeResult(True, f"Collection '{collection}' dropped")

    def list_collections(self) -> list[str]:
        return sorted(self.collections)

    def collection_exists(self, collection: str) -> bool:
        return collection in self.collections

    def insert_one(self, collection: str, document_json: str) -> FakeResult:
        if collection not in self.collections:
            return FakeResult(False, error_msg=f"Collection '{collection}' does not exist")
        document = json.loads(document_json)
        document_id = str(document.get("_id"))
        self.collections[collection][document_id] = document
        return FakeResult(True, document_id)

    def find_by_id(self, collection: str, document_id: str) -> FakeResult:
        if collection not in self.collections:
            return FakeResult(False, error_msg=f"Collection '{collection}' does not exist")
        document = self.collections[collection].get(document_id)
        if document is None:
            return FakeResult(
                False,
                error_msg=f"Document '{document_id}' not found in '{collection}'",
            )
        return FakeResult(True, json.dumps(document))

    def find_many(self, collection: str) -> FakeResult:
        if collection not in self.collections:
            return FakeResult(False, error_msg=f"Collection '{collection}' does not exist")
        return FakeResult(True, json.dumps(list(self.collections[collection].values())))

    def count(self, collection: str) -> FakeResult:
        if collection not in self.collections:
            return FakeResult(False, error_msg=f"Collection '{collection}' does not exist")
        return FakeResult(True, str(len(self.collections[collection])))

    def delete_by_id(self, collection: str, document_id: str) -> FakeResult:
        if collection not in self.collections:
            return FakeResult(False, error_msg=f"Collection '{collection}' does not exist")
        if document_id not in self.collections[collection]:
            return FakeResult(True, "0")
        self.collections[collection].pop(document_id)
        return FakeResult(True, "1")


def test_register_login_and_me_use_internal_user_store() -> None:
    engine = FakeInternalUserEngine()
    settings = AdminApiSettings(auth_secret="x" * 48)
    client = TestClient(create_app(settings=settings, engine=engine))

    setup_before = client.get("/auth/setup-state")
    assert setup_before.status_code == 200
    assert setup_before.json() == {"needsSetup": True}

    register_response = client.post(
        "/auth/register",
        json={
            "firstName": "Database",
            "lastName": "Administrator",
            "email": "admin@example.com",
            "password": "StrongPass123",
            "confirmPassword": "StrongPass123",
        },
    )
    assert register_response.status_code == 201
    assert register_response.json()["username"] == "root"
    assert engine.users["root"]["password_hash"].startswith("$argon2id$")
    assert "password_hash" not in register_response.text

    setup_after = client.get("/auth/setup-state")
    assert setup_after.status_code == 200
    assert setup_after.json() == {"needsSetup": False}

    login_response = client.post(
        "/auth/login",
        json={"username": "root", "password": "StrongPass123"},
    )
    assert login_response.status_code == 200
    token = login_response.json()["accessToken"]
    assert login_response.json()["user"]["lastLoginAt"] is not None

    me_response = client.get("/auth/me", headers={"Authorization": f"Bearer {token}"})
    assert me_response.status_code == 200
    assert me_response.json()["username"] == "root"


def test_login_rejects_wrong_password() -> None:
    engine = FakeInternalUserEngine()
    settings = AdminApiSettings(auth_secret="x" * 48)
    client = TestClient(create_app(settings=settings, engine=engine))
    client.post(
        "/auth/register",
        json={
            "firstName": "Database",
            "lastName": "Administrator",
            "email": "admin@example.com",
            "password": "StrongPass123",
            "confirmPassword": "StrongPass123",
        },
    )

    response = client.post("/auth/login", json={"username": "root", "password": "wrong"})

    assert response.status_code == 401
    assert response.json()["message"] == "invalid username or password"


def test_created_app_tokens_are_persisted_and_listed() -> None:
    engine = FakeInternalUserEngine()
    settings = AdminApiSettings(auth_secret="x" * 48, api_token_secret="y" * 48)
    client = TestClient(create_app(settings=settings, engine=engine))
    client.post(
        "/auth/register",
        json={
            "firstName": "Database",
            "lastName": "Administrator",
            "email": "admin@example.com",
            "password": "StrongPass123",
            "confirmPassword": "StrongPass123",
        },
    )
    login_response = client.post(
        "/auth/login",
        json={"username": "root", "password": "StrongPass123"},
    )
    headers = {"Authorization": f"Bearer {login_response.json()['accessToken']}"}

    created = client.post(
        "/apps/tokens",
        headers=headers,
        json={
            "appId": "reporting",
            "appName": "Reporting service",
            "scopes": ["query:execute", "collections:read"],
            "expiresInSeconds": 3600,
        },
    )
    assert created.status_code == 200

    listed = client.get("/apps/tokens", headers=headers)
    assert listed.status_code == 200
    assert len(listed.json()) == 1
    stored = listed.json()[0]
    assert stored["appId"] == "reporting"
    assert stored["appName"] == "Reporting service"
    assert stored["token"] == created.json()["token"]
    assert stored["scopes"] == ["collections:read", "query:execute"]
    assert stored["expiresAt"] == created.json()["expiresAt"]
    assert stored["status"] == "active"
    token_id = stored["id"]
    assert created.json()["token"] not in json.dumps(engine.app_tokens[token_id])

    deleted = client.delete(f"/apps/tokens/{token_id}", headers=headers)
    assert deleted.status_code == 204
    assert client.get("/apps/tokens", headers=headers).json() == []


def test_monitoring_metrics_include_recent_active_connections(tmp_path) -> None:
    engine = FakeInternalUserEngine()
    settings = AdminApiSettings(auth_secret="x" * 48, db_path=tmp_path / "db")
    state = MonitoringState()
    server = MonitoringSocketServer(
        settings=settings,
        state=state,
        engine_provider=lambda: engine,
    )

    async def collect() -> dict:
        await state.record_request(
            client_id="http:root",
            address="127.0.0.1",
            user="root",
        )
        return await server.collect_metrics()

    metrics = asyncio.run(collect())

    assert metrics["databaseHealthy"] is True
    assert metrics["databaseEngineHealthy"] is True
    assert metrics["requestsPerSecond"] == 1
    assert metrics["ramUsedBytes"] == 123_456
    assert metrics["ssdUsedBytes"] == 654_321
    assert metrics["metricSources"]["databaseHealthy"] == "nexoradb.so:is_healthy"
    assert metrics["metricSources"]["ramUsedBytes"] == "nexoradb.so:get_ram_usage_bytes"
    assert metrics["metricSources"]["ssdUsedBytes"] == "nexoradb.so:get_disk_usage_bytes"
    assert metrics["activeConnections"][0]["id"] == "http:root"
    assert metrics["activeConnections"][0]["activeWithinSeconds"] == 10.0


def test_collection_and_document_crud_routes_require_admin_token() -> None:
    engine = FakeInternalUserEngine()
    settings = AdminApiSettings(auth_secret="x" * 48)
    client = TestClient(create_app(settings=settings, engine=engine))
    client.post(
        "/auth/register",
        json={
            "firstName": "Database",
            "lastName": "Administrator",
            "email": "admin@example.com",
            "password": "StrongPass123",
            "confirmPassword": "StrongPass123",
        },
    )
    login_response = client.post(
        "/auth/login",
        json={"username": "root", "password": "StrongPass123"},
    )
    headers = {"Authorization": f"Bearer {login_response.json()['accessToken']}"}

    create_collection_response = client.post(
        "/collections",
        json={"name": "users"},
        headers=headers,
    )
    assert create_collection_response.status_code == 201
    assert create_collection_response.json()["name"] == "users"

    create_document_response = client.post(
        "/collections/users/documents",
        json={"data": {"username": "ali", "age": 28}},
        headers=headers,
    )
    assert create_document_response.status_code == 201
    created_document = create_document_response.json()
    document_id = created_document["id"]
    assert created_document["data"]["_id"] == document_id

    update_document_response = client.put(
        f"/collections/users/documents/{document_id}",
        json={"data": {"_id": document_id, "username": "sara", "age": 29}},
        headers=headers,
    )
    assert update_document_response.status_code == 200
    assert update_document_response.json()["data"]["username"] == "sara"

    rename_response = client.put(
        "/collections/users",
        json={"name": "members"},
        headers=headers,
    )
    assert rename_response.status_code == 200
    assert rename_response.json()["name"] == "members"

    list_documents_response = client.get("/collections/members/documents", headers=headers)
    assert list_documents_response.status_code == 200
    assert list_documents_response.json()[0]["id"] == document_id

    delete_document_response = client.delete(
        f"/collections/members/documents/{document_id}",
        headers=headers,
    )
    assert delete_document_response.status_code == 204

    delete_collection_response = client.delete("/collections/members", headers=headers)
    assert delete_collection_response.status_code == 204


def test_graph_crud_routes_use_graph_manager_and_metadata(tmp_path) -> None:
    engine = FakeInternalUserEngine()
    settings = AdminApiSettings(auth_secret="x" * 48, graph_dir=tmp_path / "graphs")
    app = create_app(settings=settings, engine=engine)
    app.state.native_module = FakeNativeModule()
    app.state.graph_manager = FakeGraphManager()
    app.state.graph_metadata_store = GraphMetadataStore(settings.graph_dir)
    client = TestClient(app)
    client.post(
        "/auth/register",
        json={
            "firstName": "Database",
            "lastName": "Administrator",
            "email": "admin@example.com",
            "password": "StrongPass123",
            "confirmPassword": "StrongPass123",
        },
    )
    login_response = client.post(
        "/auth/login",
        json={"username": "root", "password": "StrongPass123"},
    )
    headers = {"Authorization": f"Bearer {login_response.json()['accessToken']}"}

    create_graph_response = client.post(
        "/graphs",
        json={"name": "social graph", "description": "People and follows"},
        headers=headers,
    )
    assert create_graph_response.status_code == 201
    graph = create_graph_response.json()
    assert graph["id"] == "social_graph"
    assert graph["name"] == "social graph"

    create_node_response = client.post(
        "/graphs/social_graph/nodes",
        json={"label": "Admin", "type": "User", "data": {"role": "admin"}},
        headers=headers,
    )
    assert create_node_response.status_code == 200
    first_node = create_node_response.json()["nodes"][0]

    second_node_response = client.post(
        "/graphs/social_graph/nodes",
        json={"label": "App", "type": "Service", "data": {}},
        headers=headers,
    )
    second_node = second_node_response.json()["nodes"][1]

    edge_response = client.post(
        "/graphs/social_graph/edges",
        json={
            "source": first_node["id"],
            "target": second_node["id"],
            "label": "connects",
            "data": {"since": "today"},
        },
        headers=headers,
    )
    assert edge_response.status_code == 200
    edge = edge_response.json()["edges"][0]

    update_node_response = client.put(
        f"/graphs/social_graph/nodes/{first_node['id']}",
        json={"label": "Root", "type": "User", "data": {"role": "root"}},
        headers=headers,
    )
    assert update_node_response.status_code == 200
    assert update_node_response.json()["nodes"][0]["label"] == "Root"

    delete_edge_response = client.delete(
        f"/graphs/social_graph/edges/{edge['id']}",
        headers=headers,
    )
    assert delete_edge_response.status_code == 200
    assert delete_edge_response.json()["edges"] == []

    delete_graph_response = client.delete("/graphs/social_graph", headers=headers)
    assert delete_graph_response.status_code == 204
