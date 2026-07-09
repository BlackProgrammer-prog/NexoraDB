from __future__ import annotations

import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


class NexoraAdminClientError(RuntimeError):
    pass


class NexoraAdminAuthError(NexoraAdminClientError):
    pass


@dataclass(frozen=True)
class AuthSession:
    access_token: str
    username: str
    expires_in: int


class AdminApiClient:
    def __init__(self, base_url: str, timeout: float = 10.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def setup_state(self) -> dict[str, Any]:
        return self._request("GET", "/auth/setup-state")

    def login(self, username: str, password: str) -> AuthSession:
        payload = self._request("POST", "/auth/login", {"username": username, "password": password})
        user = payload.get("user") if isinstance(payload.get("user"), dict) else {}
        return AuthSession(
            access_token=str(payload["accessToken"]),
            username=str(user.get("username") or username),
            expires_in=int(payload.get("expiresIn") or 0),
        )

    def register(
        self,
        *,
        first_name: str,
        last_name: str,
        email: str,
        password: str,
    ) -> None:
        self._request(
            "POST",
            "/auth/register",
            {
                "firstName": first_name,
                "lastName": last_name,
                "email": email,
                "password": password,
                "confirmPassword": password,
            },
        )

    def execute_query(self, token: str, query: str) -> dict[str, Any]:
        return self._request("POST", "/query/execute", {"query": query}, token=token)

    def create_app_token(
        self,
        token: str,
        *,
        app_id: str,
        app_name: str | None,
        expires_in_seconds: int | None,
        scopes: list[str],
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            "/apps/tokens",
            {
                "appId": app_id,
                "appName": app_name,
                "scopes": scopes,
                "expiresInSeconds": expires_in_seconds,
            },
            token=token,
        )

    def list_app_scopes(self, token: str) -> list[str]:
        payload = self._request("GET", "/apps/scopes", token=token)
        scopes = payload.get("scopes")
        return [str(scope) for scope in scopes] if isinstance(scopes, list) else []

    def _request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
        *,
        token: str | None = None,
    ) -> dict[str, Any]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"Content-Type": "application/json", "User-Agent": "nexoradb-cli/0.1"}
        if token:
            headers["Authorization"] = f"Bearer {token}"

        request = urllib.request.Request(
            f"{self.base_url}{path}",
            data=data,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read().decode("utf-8")
                return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as exc:
            message = _read_error_message(exc)
            if exc.code in {401, 403}:
                raise NexoraAdminAuthError(message) from exc
            raise NexoraAdminClientError(message) from exc
        except urllib.error.URLError as exc:
            raise NexoraAdminClientError(str(exc.reason)) from exc


def _read_error_message(exc: urllib.error.HTTPError) -> str:
    try:
        payload = json.loads(exc.read().decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return f"Request failed with status {exc.code}"
    if isinstance(payload, dict):
        return str(payload.get("message") or payload.get("detail") or f"Request failed with status {exc.code}")
    return f"Request failed with status {exc.code}"
