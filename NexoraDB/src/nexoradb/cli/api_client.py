"""
NexoraDB Admin API client.
مسیر: src/nexoradb/cli/api_client.py
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

import httpx

from .query_client import strict_json


class NexoraAdminClientError(Exception):
    """هر خطایی که از API برمی‌گردد."""


@dataclass
class AuthSession:
    access_token: str
    username: str


class AdminApiClient:
    """
    HTTP client ساده برای Admin API.
    Responses are bounded; redirects, cookies and environment proxies are disabled.
    """

    def __init__(self, base_url: str) -> None:
        self.base_url = base_url.rstrip("/")
        url = httpx.URL(self.base_url)
        if (
            url.scheme not in {"http", "https"}
            or not url.host
            or url.userinfo
            or url.query
            or url.fragment
        ):
            raise ValueError(
                "Expected an HTTP(S) server URL without credentials, query or fragment"
            )

    # ── internal ────────────────────────────────────────────────────────────

    def _request(
        self,
        method: str,
        path: str,
        *,
        token: str | None = None,
        json_body: Any = None,
    ) -> Any:
        if not path.startswith("/") or path.startswith("//") or chr(92) in path:
            raise NexoraAdminClientError("Invalid API path")
        url = self.base_url + path
        headers = {"Content-Type": "application/json", "Accept-Encoding": "identity"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        try:
            data = None if json_body is None else json.dumps(json_body, allow_nan=False).encode()
            if data is not None and len(data) > 1024 * 1024:
                raise NexoraAdminClientError("Request exceeds 1 MiB")
            with httpx.Client(follow_redirects=False, trust_env=False, timeout=15) as client:
                with client.stream(method, url, content=data, headers=headers) as response:
                    if response.headers.get("content-encoding", "identity") != "identity":
                        raise NexoraAdminClientError("Unexpected compressed response")
                    limit = 8 * 1024 * 1024 if response.is_success else 65536
                    raw = bytearray()
                    for chunk in response.iter_bytes():
                        if len(raw) + len(chunk) > limit:
                            raise NexoraAdminClientError(
                                "Response exceeds the terminal byte budget"
                            )
                        raw.extend(chunk)
                    if not response.is_success:
                        try:
                            detail = strict_json(raw)
                            message = detail.get("message") or detail.get("detail")
                            if isinstance(message, dict):
                                message = message.get("message")
                        except (ValueError, AttributeError):
                            message = None
                        raise NexoraAdminClientError(
                            message
                            if isinstance(message, str)
                            else f"API request failed ({response.status_code})"
                        )
                    if not raw:
                        return {}
                    if "application/json" not in response.headers.get("content-type", "").lower():
                        raise NexoraAdminClientError("Expected a JSON response")
                    return strict_json(raw)
        except NexoraAdminClientError:
            raise
        except (httpx.HTTPError, ValueError, TypeError, RecursionError) as exc:
            raise NexoraAdminClientError(
                "Database transport failed; reconcile writes before retrying"
            ) from exc

    # ── auth ────────────────────────────────────────────────────────────────

    def setup_state(self) -> dict[str, Any]:
        """GET /auth/setup-state — بررسی می‌کند آیا root admin وجود دارد."""
        return self._request("GET", "/auth/setup-state")

    def login(self, username: str, password: str) -> AuthSession:
        """POST /auth/login"""
        resp = self._request(
            "POST",
            "/auth/login",
            json_body={"username": username, "password": password},
        )
        token = resp.get("accessToken") or resp.get("access_token") or resp.get("token")
        if not token:
            raise NexoraAdminClientError("No access token in login response.")
        return AuthSession(access_token=token, username=username)

    def register(
        self,
        *,
        first_name: str,
        last_name: str,
        email: str,
        password: str,
    ) -> dict[str, Any]:
        """POST /auth/register — ساخت اولین root admin."""
        return self._request(
            "POST",
            "/auth/register",
            json_body={
                "firstName": first_name,
                "lastName": last_name,
                "email": email,
                "password": password,
            },
        )

    # ── app tokens ──────────────────────────────────────────────────────────

    def list_app_scopes(self, token: str) -> list[str]:
        """GET /apps/scopes"""
        resp = self._request("GET", "/apps/scopes", token=token)
        return resp.get("scopes", [])

    def create_app_token(
        self,
        token: str,
        *,
        app_id: str,
        app_name: str | None = None,
        expires_in_seconds: int | None = None,
        scopes: list[str],
    ) -> dict[str, Any]:
        """POST /apps/tokens"""
        return self._request(
            "POST",
            "/apps/tokens",
            token=token,
            json_body={
                "appId": app_id,
                "appName": app_name,
                "expiresInSeconds": expires_in_seconds,
                "scopes": scopes,
            },
        )

    # ── query ───────────────────────────────────────────────────────────────

    def execute_query(self, token: str, query: str) -> dict[str, Any]:
        """POST /query/execute"""
        resp = self._request(
            "POST",
            "/query/execute",
            token=token,
            json_body={"query": query},
        )
        # نرمال‌سازی خروجی برای DataTable
        rows = resp.get("rows", [])
        columns: list[str] = resp.get("columns", [])
        if not columns and rows:
            columns = list(rows[0].keys()) if isinstance(rows[0], dict) else []
        return {
            "columns": columns,
            "rows": rows,
            "executionTimeMs": resp.get("executionTimeMs", 0),
            "raw": resp,
            "statementErrors": resp.get("statementErrors", []),
        }

    # ── graphs ──────────────────────────────────────────────────────────────

    def list_graphs(self, token: str) -> list[str]:
        """GET /graphs — لیست نام گراف‌های موجود."""
        try:
            resp = self._request("GET", "/graphs", token=token)
            # API یک list[dict] برمی‌گرداند؛ هر dict دارای فیلد "name" است
            if isinstance(resp, list):
                return [g.get("id", g.get("name", str(g))) for g in resp if isinstance(g, dict)]
            return []
        except NexoraAdminClientError:
            # اگر graph module فعال نبود، خطا نمی‌دهیم — فقط لیست خالی
            return []

    def run_graph_algorithm(
        self,
        token: str,
        algo_id: str,
        graph_name: str,
        params: list[str],
    ) -> dict[str, Any]:
        """Run one of the dashboard algorithms through the real NexoraQL endpoint."""
        from .console_api import graph_query

        try:
            query, bindings = graph_query(algo_id, graph_name, params)
        except ValueError as exc:
            raise NexoraAdminClientError(str(exc)) from exc
        response = self._request(
            "POST",
            "/query/execute",
            token=token,
            json_body={"query": query, "parameters": bindings},
        )
        if response.get("statementErrors"):
            raise NexoraAdminClientError("Algorithm statement failed")
        return {
            "result": response,
            "query": query,
            "parameters": bindings,
            "elapsedMs": response.get("executionTimeMs", 0),
        }
