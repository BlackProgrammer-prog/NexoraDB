from __future__ import annotations

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
