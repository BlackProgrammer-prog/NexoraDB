"""
NexoraDB Admin API client.
مسیر: src/nexoradb/cli/api_client.py
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

import urllib.request
import urllib.error


class NexoraAdminClientError(Exception):
    """هر خطایی که از API برمی‌گردد."""


@dataclass
class AuthSession:
    access_token: str
    username: str


class AdminApiClient:
    """
    HTTP client ساده برای Admin API.
    از urllib استاندارد Python استفاده می‌کند تا نیازی به httpx/requests نباشد.
    """

    def __init__(self, base_url: str) -> None:
        self.base_url = base_url.rstrip("/")

    # ── internal ────────────────────────────────────────────────────────────

    def _request(
        self,
        method: str,
        path: str,
        *,
        token: str | None = None,
        json_body: Any = None,
    ) -> Any:
        url = self.base_url + path
        data: bytes | None = None
        headers: dict[str, str] = {"Content-Type": "application/json"}

        if token:
            headers["Authorization"] = f"Bearer {token}"

        if json_body is not None:
            data = json.dumps(json_body).encode()

        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                raw = resp.read()
                if not raw:
                    return {}
                return json.loads(raw)
        except urllib.error.HTTPError as exc:
            body = exc.read()
            try:
                detail = json.loads(body)
                msg = detail.get("message") or detail.get("detail") or str(detail)
            except Exception:
                msg = body.decode(errors="replace") or f"HTTP {exc.code}"
            raise NexoraAdminClientError(msg) from exc
        except urllib.error.URLError as exc:
            raise NexoraAdminClientError(f"Cannot reach {self.base_url}: {exc.reason}") from exc
        except Exception as exc:
            raise NexoraAdminClientError(str(exc)) from exc

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
        }

    # ── graphs ──────────────────────────────────────────────────────────────

    def list_graphs(self, token: str) -> list[str]:
        """GET /graphs — لیست نام گراف‌های موجود."""
        try:
            resp = self._request("GET", "/graphs", token=token)
            # API یک list[dict] برمی‌گرداند؛ هر dict دارای فیلد "name" است
            if isinstance(resp, list):
                return [g.get("name", g.get("id", str(g))) for g in resp if isinstance(g, dict)]
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
        """
        اجرای یک الگوریتم روی گراف.
        چون endpoint رسمی هنوز وجود ندارد، از query/execute استفاده می‌کنیم.
        وقتی endpoint اضافه شد فقط این متد تغییر می‌کند.
        """
        # تلاش اول: endpoint اختصاصی (اگر بعداً اضافه شد)
        try:
            return self._request(
                "POST",
                "/graphs/algorithm",
                token=token,
                json_body={
                    "graphName": graph_name,
                    "algorithm": algo_id,
                    "params": params,
                },
            )
        except NexoraAdminClientError:
            pass

        # fallback: از NexoraQL query استفاده می‌کنیم
        params_str = ", ".join(f'"{p}"' for p in params)
        graph_clause = f'GRAPH "{graph_name}"' if graph_name else ""
        query = f"RUN ALGORITHM {algo_id} {graph_clause} WITH PARAMS [{params_str}];"
        try:
            resp = self._request(
                "POST",
                "/query/execute",
                token=token,
                json_body={"query": query},
            )
            return {"result": resp, "elapsedMs": resp.get("executionTimeMs", 0)}
        except NexoraAdminClientError as exc:
            # اگر هر دو روش شکست خوردند، خطا را propagate می‌کنیم
            raise NexoraAdminClientError(
                f"Algorithm '{algo_id}' failed: {exc}\n"
                "(endpoint /graphs/algorithm وجود ندارد و NexoraQL هم پشتیبانی نمی‌کند)"
            ) from exc
