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
from nexoradb_admin.monitoring import MonitoringSocketServer, MonitoringState  # noqa: E402


@dataclass
class FakeResult:
    success: bool
    data: str = ""
    error_msg: str = ""


class FakeInternalUserEngine:
    def __init__(self) -> None:
        self.users: dict[str, dict] = {}

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

    def is_healthy(self) -> bool:
        return True

    def get_ram_usage_bytes(self) -> int:
        return 123_456

    def get_disk_usage_bytes(self) -> int:
        return 654_321


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
