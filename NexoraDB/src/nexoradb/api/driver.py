from __future__ import annotations

import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


class NexoraDBError(RuntimeError):
    """Base driver error for NexoraDB API calls."""


class NexoraDBAuthError(NexoraDBError):
    """Raised when the API rejects the application token."""


@dataclass(frozen=True)
class QueryResult:
    columns: list[str]
    rows: list[dict[str, Any]]
    raw: Any
    execution_time_ms: int


class NexoraDBClient:
    """Small Python driver for connecting external apps to the NexoraDB API."""

    def __init__(self, *, url: str, token: str, timeout: float = 10.0) -> None:
        self.url = url.rstrip("/")
        self.token = token
        self.timeout = timeout

    def execute(self, query: str) -> QueryResult:
        payload = self._request("POST", "/api/v1/query", {"query": query})
        return QueryResult(
            columns=[str(column) for column in payload.get("columns", [])],
            rows=list(payload.get("rows", [])),
            raw=payload.get("raw"),
            execution_time_ms=int(payload.get("executionTimeMs", 0)),
        )

    def ping(self) -> bool:
        payload = self._request("GET", "/api/v1/health", None)
        return payload.get("message") == "ok"

    def create_collection(self, name: str) -> QueryResult:
        return self.execute(f"CREATE COLLECTION {name};")

    def list_collections(self) -> QueryResult:
        return self.execute("SHOW COLLECTIONS;")

    def insert_one(self, collection: str, document: dict[str, Any]) -> QueryResult:
        document_json = json.dumps(document, separators=(",", ":"))
        return self.execute(f"INSERT INTO {collection} VALUES ({_quote(document_json)});")

    def find(self, collection: str, *, limit: int = 100) -> QueryResult:
        return self.execute(f"SELECT * FROM {collection} LIMIT {limit};")

    def count(self, collection: str) -> QueryResult:
        return self.execute(f"COUNT FROM {collection};")

    def _request(self, method: str, path: str, body: dict[str, Any] | None) -> dict[str, Any]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(
            f"{self.url}{path}",
            data=data,
            method=method,
            headers={
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/json",
                "User-Agent": "nexoradb-python-driver/0.1",
            },
        )

        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            message = _error_message(exc)
            if exc.code in {401, 403}:
                raise NexoraDBAuthError(message) from exc
            raise NexoraDBError(message) from exc
        except urllib.error.URLError as exc:
            raise NexoraDBError(str(exc.reason)) from exc


def connect(*, url: str = "http://localhost:8000", token: str, timeout: float = 10.0) -> NexoraDBClient:
    return NexoraDBClient(url=url, token=token, timeout=timeout)


def _error_message(exc: urllib.error.HTTPError) -> str:
    try:
        payload = json.loads(exc.read().decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return f"NexoraDB API request failed with status {exc.code}"

    message = payload.get("message") if isinstance(payload, dict) else None
    return str(message or f"NexoraDB API request failed with status {exc.code}")


def _quote(value: str) -> str:
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"
