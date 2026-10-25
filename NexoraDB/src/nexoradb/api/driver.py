from __future__ import annotations

import json
import re
import http.client
import httpx
import ssl
import os
from contextlib import contextmanager
from threading import BoundedSemaphore, Lock
import time
from urllib.parse import urlsplit
from dataclasses import dataclass
from typing import Any


class NexoraDBError(RuntimeError):
    """Base driver error for NexoraDB API calls."""


class NexoraDBAuthError(NexoraDBError):
    """Raised when the API rejects the application token."""


class NexoraDBOutcomeUnknown(NexoraDBError):
    """A POST may have committed; never blindly repeat it with a new key."""


class NexoraDBPoolBusy(NexoraDBError):
    """No connection became available; this request was not sent."""


@dataclass(frozen=True)
class QueryResult:
    columns: list[str]
    rows: list[dict[str, Any]]
    raw: Any
    execution_time_ms: int


class NexoraDBClient:
    """Small Python driver for connecting external apps to the NexoraDB API."""

    def __init__(self, *, url: str, token: str, timeout: float = 10.0,
                 max_connections: int = 16, pool_timeout: float = 2.0, http2: bool = False,
                 ssl_context: ssl.SSLContext | None = None) -> None:
        parsed = urlsplit(url)
        if (parsed.scheme not in {"http", "https"} or not parsed.hostname
                or parsed.username or parsed.password or parsed.query or parsed.fragment):
            raise ValueError("Invalid database API URL")
        if parsed.scheme == "http" and parsed.hostname not in {"localhost", "127.0.0.1", "::1"}:
            raise ValueError("Remote database connections require HTTPS")
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        if not token or any(c in token for c in "\r\n"):
            raise ValueError("Invalid bearer token")
        if not 1 <= max_connections <= 256 or pool_timeout <= 0:
            raise ValueError("Invalid connection pool limits")
        if ssl_context is not None and (not ssl_context.check_hostname or ssl_context.verify_mode != ssl.CERT_REQUIRED):
            raise ValueError("TLS hostname and certificate verification must remain enabled")
        self.url = url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self._slots = BoundedSemaphore(max_connections)
        self._pool_timeout = pool_timeout
        self._pid = os.getpid()
        self._max_connections = max_connections
        self._metrics_lock = Lock()
        self._active = self._pool_timeouts = self._released = 0
        self._wait_seconds = 0.0
        self._client = httpx.Client(
            limits=httpx.Limits(max_connections=max_connections,
                               max_keepalive_connections=min(max_connections, 8), keepalive_expiry=30),
            timeout=httpx.Timeout(timeout, pool=pool_timeout), http2=http2,
            follow_redirects=False, trust_env=False,
            verify=ssl_context if ssl_context is not None else True,
        )

    def close(self):
        self._client.close()

    def pool_stats(self):
        with self._metrics_lock:
            return dict(max_connections=self._max_connections, active=self._active,
                        acquisition_timeouts=self._pool_timeouts, released_leases=self._released,
                        acquisition_wait_seconds=self._wait_seconds, closed=self._client.is_closed)

    @contextmanager
    def _lease(self):
        if os.getpid() != self._pid:
            raise NexoraDBError("Create a new client after fork; inherited pools are unsafe")
        started = time.monotonic()
        acquired = self._slots.acquire(timeout=self._pool_timeout)
        with self._metrics_lock:
            self._wait_seconds += time.monotonic() - started
            if acquired:
                self._active += 1
            else:
                self._pool_timeouts += 1
        if not acquired:
            raise NexoraDBPoolBusy("Client concurrency exhausted; request not sent")
        try:
            yield
        finally:
            with self._metrics_lock:
                self._active -= 1
                self._released += 1
            self._slots.release()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def execute(self, query: str, parameters: dict[str, Any] | None = None, *,
                idempotency_key: str | None = None, include_raw: bool = True,
                binary: bool = False) -> QueryResult:
        body = {"query": query}
        if parameters is not None:
            body["parameters"] = parameters
        if not include_raw:
            body["include_raw"] = False
        if idempotency_key is not None:
            if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", idempotency_key):
                raise ValueError("Invalid Idempotency-Key")
            body["_idempotency_key"] = idempotency_key
        payload = self._request("POST", "/api/v1/query/bson" if binary else "/api/v1/query", body)
        if payload.get("statementErrors"):
            raise NexoraDBError(str(payload["statementErrors"][0]))
        raw = payload.get("raw")
        if isinstance(raw, dict):
            for result in raw.get("statements", []):
                if result.get("success") is False:
                    raise NexoraDBError(result.get("error") or "Query statement failed")
        return QueryResult(
            columns=[str(column) for column in payload.get("columns", [])],
            rows=list(payload.get("rows", [])),
            raw=payload.get("raw"),
            execution_time_ms=int(payload.get("executionTimeMs", 0)),
        )

    def ping(self) -> bool:
        payload = self._request("GET", "/api/v1/health", None)
        return payload.get("message") == "ok"

    async def aexecute(self, query: str, parameters: dict[str, Any] | None = None, **options) -> QueryResult:
        """Thread-backed async convenience; uses the existing bounded pool.

        Wait for in-flight work on cancellation; never abandon a write and
        automatically retry. Use an idempotency key for write reconciliation.
        This does not share an embedded TxHandle between worker threads.
        """
        import anyio
        from functools import partial
        return await anyio.to_thread.run_sync(partial(self.execute, query, parameters, **options),
                                              abandon_on_cancel=False)

    def create_collection(self, name: str) -> QueryResult:
        return self.execute(f"CREATE COLLECTION {_identifier(name)};")

    def list_collections(self) -> QueryResult:
        return self.execute("SHOW COLLECTIONS;")

    def insert_one(self, collection: str, document: dict[str, Any]) -> QueryResult:
        return self.execute(f"INSERT INTO {_identifier(collection)} VALUES ($document);",
                            {"document": document})

    def insert_many(self, collection: str, documents: list[dict[str, Any]], *,
                    idempotency_key: str | None = None, binary: bool = False) -> QueryResult:
        """One bounded parameterized request, not one RPC/commit per document.

        Native ordinary collections retain atomic batch semantics. No automatic
        splitting or retry: callers must handle an unknown write outcome explicitly.
        """
        name = _identifier(collection)
        if type(documents) is not list or not 1 <= len(documents) <= 1000:
            raise ValueError('insert_many requires a list of 1..1000 documents')
        if any(type(document) is not dict for document in documents):
            raise ValueError('Every document must be an object')
        parameters = {f'doc_{i}': document for i, document in enumerate(documents)}
        slots = ','.join(f'($doc_{i})' for i in range(len(documents)))
        return self.execute(f'INSERT INTO {name} BATCH VALUES {slots};', parameters,
                            idempotency_key=idempotency_key, **({'binary': True} if binary else {}))

    def find(self, collection: str, *, limit: int = 100) -> QueryResult:
        if type(limit) is not int or not 0 <= limit <= 0xffffffff:
            raise ValueError("limit must be an unsigned 32-bit integer")
        return self.execute(f"SELECT * FROM {_identifier(collection)} LIMIT $limit;", {"limit": limit})

    def count(self, collection: str) -> QueryResult:
        return self.execute(f"COUNT FROM {_identifier(collection)};")

    def _request(self, method: str, path: str, body: dict[str, Any] | None) -> dict[str, Any]:
        body = dict(body) if body is not None else None
        key = body.pop("_idempotency_key", None) if body else None
        data = None if body is None else json.dumps(body, allow_nan=False).encode("utf-8")
        binary = path == '/api/v1/query/bson'
        if binary:
            try:
                from bson import BSON
            except ImportError as exc:
                raise NexoraDBError('Install nexoradb[binary] for BSON request encoding') from exc
            data = BSON.encode(body)
        if data is not None and len(data)>2*1024*1024:
            raise ValueError('Request exceeds 2MiB; split into explicit application batches')
        headers = {
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/bson" if binary else "application/json",
                "User-Agent": "nexoradb-python-driver/0.3",
            }
        if key:
            headers["Idempotency-Key"] = key

        try:
            with self._lease(), self._client.stream(method, f"{self.url}{path}", content=data, headers=headers) as response:
                self._check_status(response)
                encoded = bytearray()
                for chunk in response.iter_bytes(64 * 1024):
                    encoded.extend(chunk)
                    if len(encoded) > 32 * 1024 * 1024:
                        if method == "POST":
                            raise NexoraDBOutcomeUnknown("Response too large; request may have committed")
                        raise NexoraDBError("Response exceeds 32MiB")
                return json.loads(encoded)
        except httpx.PoolTimeout as exc:
            raise NexoraDBPoolBusy("Pool acquisition timeout; request was not sent") from exc
        except (httpx.TransportError, TimeoutError, ConnectionError, http.client.HTTPException,
                json.JSONDecodeError, UnicodeDecodeError) as exc:
            if method == "POST":
                raise NexoraDBOutcomeUnknown("Request outcome unknown; reconcile or retry the SAME Idempotency-Key") from exc
            raise NexoraDBError("Database transport failed") from exc

    @staticmethod
    def _check_status(response):
        if 200 <= response.status_code < 300:
            return
        if response.status_code in {401, 403}:
            raise NexoraDBAuthError(f"API authorization failed ({response.status_code})")
        raise NexoraDBError(f"API request failed ({response.status_code}); redirects are not followed")

    @contextmanager
    def cursor(self, query: str, parameters=None, *, batch_size: int = 128):
        """Use `with client.cursor(...) as rows` and consume lazily.

        SELECT-only streaming is never retried/restarted implicitly: a new
        stream can observe a different snapshot.
        """
        if not 1 <= batch_size <= 1000:
            raise ValueError("batch_size must be 1..1000")
        headers = {"Authorization": f"Bearer {self.token}", "Content-Type": "application/json"}
        body = json.dumps({"query": query, "parameters": parameters or {}}, allow_nan=False)
        try:
            with self._lease(), self._client.stream("POST", f"{self.url}/api/v1/query/cursor?batch_size={batch_size}",
                                     content=body.encode(), headers=headers) as response:
                self._check_status(response)
                def rows():
                    buffer = bytearray()
                    for chunk in response.iter_bytes(64 * 1024):
                        buffer.extend(chunk)
                        while True:
                            boundary = buffer.find(b"\n")
                            if boundary < 0:
                                if len(buffer) > 2 * 1024 * 1024:
                                    raise NexoraDBError("Cursor record exceeds 2MiB")
                                break
                            if boundary > 2 * 1024 * 1024:
                                raise NexoraDBError("Cursor record exceeds 2MiB")
                            record = json.loads(buffer[:boundary])
                            del buffer[:boundary + 1]
                            if record.get("end") is True:
                                return
                            if "document" not in record:
                                raise NexoraDBError("Cursor failed; incomplete result")
                            yield record["document"]
                    raise NexoraDBError("Cursor disconnected before completion")
                iterator = rows()
                try:
                    yield iterator
                finally:
                    iterator.close()
        except httpx.PoolTimeout as exc:
            raise NexoraDBPoolBusy("Pool acquisition timeout; cursor not sent") from exc
        except (httpx.TransportError, json.JSONDecodeError) as exc:
            raise NexoraDBError("Cursor transport failed; do not silently restart") from exc


def connect(*, url: str = "http://localhost:8000", token: str, timeout: float = 10.0,
            max_connections: int = 16, pool_timeout: float = 2, http2: bool = False,
            ssl_context: ssl.SSLContext | None = None) -> NexoraDBClient:
    return NexoraDBClient(url=url, token=token, timeout=timeout, max_connections=max_connections,
                         pool_timeout=pool_timeout, http2=http2, ssl_context=ssl_context)


def _quote(value: str) -> str:
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"


def _identifier(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        raise ValueError("Invalid collection identifier")
    return value
