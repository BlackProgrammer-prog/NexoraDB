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
        if not graph_name:
            raise NexoraAdminClientError("Select a graph before running an algorithm.")

        quote = lambda value: "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"
        p1 = quote(params[0]) if params else None
        p2 = quote(params[1]) if len(params) > 1 else None
        two_nodes = {
            "AreConnected": ("user1", "user2"),
            "MutualFriends": ("user1", "user2"),
            "ShortestPath": ("from", "to"),
        }
        one_node = {
            "GetFriends": "user",
            "FriendSuggestion": "user",
            "AllDistances": "source",
        }
        job_algorithms = {
            "ConnectedComponents", "AllDistances", "BetweennessCentrality",
            "CommunityDetection", "InfluenceMaximization",
        }
        kind = "JOB" if algo_id in job_algorithms else "LOCK"
        with_parts: list[str] = []
        if algo_id in two_nodes:
            if not p1 or not p2:
                raise NexoraAdminClientError(f"{algo_id} needs two node ids in Params.")
            first, second = two_nodes[algo_id]
            with_parts.extend((f"{first}={p1}", f"{second}={p2}"))
        elif algo_id in one_node:
            if not p1:
                raise NexoraAdminClientError(f"{algo_id} needs one node id in Params.")
            with_parts.append(f"{one_node[algo_id]}={p1}")

        if algo_id == "AllDistances":
            with_parts.extend(("all=true", "max_hops=4"))
        elif algo_id == "CommunityDetection":
            with_parts.extend(("max_iterations=10", "min_community_size=2", "members=true"))
        elif algo_id == "InfluenceMaximization":
            with_parts.extend(("k=5", "simulations=20", "probability=0.1"))

        with_clause = f" WITH {', '.join(with_parts)}" if with_parts else ""
        limit_clause = " LIMIT 20" if algo_id in {"GetFriends", "FriendSuggestion", "MostConnected"} else ""
        query = f"RUN {kind} {algo_id} ON {graph_name}{with_clause}{limit_clause};"
        try:
            resp = self._request(
                "POST",
                "/query/execute",
                token=token,
                json_body={"query": query},
            )
            return {"result": resp, "query": query, "elapsedMs": resp.get("executionTimeMs", 0)}
        except NexoraAdminClientError as exc:
            raise NexoraAdminClientError(
                f"Algorithm '{algo_id}' failed: {exc}\nQuery: {query}"
            ) from exc
