from __future__ import annotations

import asyncio
import shutil
import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, Protocol

import socketio

from .config import AdminApiSettings
from .security import TokenError, decode_access_token

ACTIVE_CONNECTION_WINDOW_SECONDS = 10.0
REQUEST_RATE_WINDOW_SECONDS = 1.0
METRICS_INTERVAL_SECONDS = 1.0


class MonitoringEngine(Protocol):
    def is_healthy(self) -> bool: ...

    def get_ram_usage_bytes(self) -> int: ...

    def get_disk_usage_bytes(self) -> int: ...


@dataclass
class ConnectionRecord:
    id: str
    address: str | None
    user: str | None
    kind: str
    last_seen: float


class MonitoringState:
    def __init__(self) -> None:
        self._request_timestamps: deque[float] = deque()
        self._connections: dict[str, ConnectionRecord] = {}
        self._lock = asyncio.Lock()

    async def record_request(
        self,
        *,
        client_id: str,
        address: str | None,
        user: str | None = None,
        kind: str = "http",
    ) -> None:
        now = time.monotonic()
        async with self._lock:
            self._request_timestamps.append(now)
            self._connections[client_id] = ConnectionRecord(
                id=client_id,
                address=address,
                user=user,
                kind=kind,
                last_seen=now,
            )
            self._prune_locked(now)

    async def touch_connection(
        self,
        *,
        client_id: str,
        address: str | None,
        user: str | None,
        kind: str,
    ) -> None:
        now = time.monotonic()
        async with self._lock:
            self._connections[client_id] = ConnectionRecord(
                id=client_id,
                address=address,
                user=user,
                kind=kind,
                last_seen=now,
            )
            self._prune_locked(now)

    async def remove_connection(self, client_id: str) -> None:
        async with self._lock:
            self._connections.pop(client_id, None)

    async def snapshot(self) -> tuple[int, list[ConnectionRecord]]:
        now = time.monotonic()
        async with self._lock:
            self._prune_locked(now)
            request_count = len(self._request_timestamps)
            connections = sorted(
                self._connections.values(),
                key=lambda connection: connection.last_seen,
                reverse=True,
            )
        return request_count, connections

    def _prune_locked(self, now: float) -> None:
        request_cutoff = now - REQUEST_RATE_WINDOW_SECONDS
        while self._request_timestamps and self._request_timestamps[0] < request_cutoff:
            self._request_timestamps.popleft()

        connection_cutoff = now - ACTIVE_CONNECTION_WINDOW_SECONDS
        stale_ids = [
            client_id
            for client_id, connection in self._connections.items()
            if connection.last_seen < connection_cutoff
        ]
        for client_id in stale_ids:
            self._connections.pop(client_id, None)


class MonitoringSocketServer:
    def __init__(
        self,
        *,
        settings: AdminApiSettings,
        state: MonitoringState,
        engine_provider: Callable[[], MonitoringEngine],
    ) -> None:
        self.settings = settings
        self.state = state
        self.engine_provider = engine_provider
        self.sio = socketio.AsyncServer(
            async_mode="asgi",
            cors_allowed_origins=list(settings.allowed_origins),
        )
        self._background_task_started = False
        self._configure_handlers()

    def asgi_app(self, fastapi_app: Any) -> socketio.ASGIApp:
        return socketio.ASGIApp(
            self.sio,
            other_asgi_app=fastapi_app,
            socketio_path="/socket.io",
        )

    async def emit_snapshot(self, sid: str | None = None) -> None:
        await self.sio.emit("metrics", await self.collect_metrics(), to=sid)

    async def collect_metrics(self) -> dict[str, Any]:
        requests_per_second, active_connections = await self.state.snapshot()

        database_engine_healthy = False
        database_metrics_available = False
        ram_used_bytes = 0
        ssd_used_bytes = 0
        try:
            engine = self.engine_provider()
            database_engine_healthy = bool(engine.is_healthy())
            ram_used_bytes = int(engine.get_ram_usage_bytes())
            ssd_used_bytes = int(engine.get_disk_usage_bytes())
            database_metrics_available = True
        except Exception:
            database_engine_healthy = False

        disk_path = self._existing_disk_usage_path()
        disk_usage = shutil.disk_usage(disk_path)
        return {
            "databaseHealthy": database_engine_healthy and database_metrics_available,
            "databaseEngineHealthy": database_engine_healthy,
            "ramUsedBytes": ram_used_bytes,
            "ssdUsedBytes": ssd_used_bytes,
            "ssdTotalBytes": disk_usage.total,
            "metricSources": {
                "databaseHealthy": "nexoradb.so:is_healthy",
                "ramUsedBytes": "nexoradb.so:get_ram_usage_bytes",
                "ssdUsedBytes": "nexoradb.so:get_disk_usage_bytes",
                "ssdTotalBytes": "host:disk_usage",
            },
            "requestsPerSecond": requests_per_second,
            "activeConnections": [
                {
                    "id": connection.id,
                    "address": connection.address,
                    "user": connection.user,
                    "kind": connection.kind,
                    "activeWithinSeconds": ACTIVE_CONNECTION_WINDOW_SECONDS,
                }
                for connection in active_connections
            ],
            "receivedAt": int(time.time() * 1000),
        }

    def _existing_disk_usage_path(self) -> str:
        path = self.settings.db_path.resolve()
        if path.exists():
            return str(path)
        for parent in path.parents:
            if parent.exists():
                return str(parent)
        return "."

    def _configure_handlers(self) -> None:
        @self.sio.event
        async def connect(sid: str, environ: dict[str, Any], auth: dict[str, Any] | None) -> bool:
            token = str((auth or {}).get("token") or "")
            try:
                payload = decode_access_token(token, self.settings)
            except TokenError:
                return False

            address = str(environ.get("REMOTE_ADDR") or "")
            await self.state.touch_connection(
                client_id=f"socket:{sid}",
                address=address or None,
                user=str(payload.get("sub") or "admin"),
                kind="socket.io",
            )
            await self.sio.save_session(
                sid,
                {"address": address or None, "user": str(payload.get("sub") or "admin")},
            )
            self._start_background_task()
            await self.emit_snapshot(sid)
            return True

        @self.sio.event
        async def disconnect(sid: str) -> None:
            await self.state.remove_connection(f"socket:{sid}")

        @self.sio.on("heartbeat")
        async def heartbeat(sid: str) -> None:
            session = await self.sio.get_session(sid)
            await self.state.touch_connection(
                client_id=f"socket:{sid}",
                address=session.get("address"),
                user=session.get("user"),
                kind="socket.io",
            )

        @self.sio.on("metrics:refresh")
        async def metrics_refresh(sid: str) -> None:
            await self.emit_snapshot(sid)

    def _start_background_task(self) -> None:
        if self._background_task_started:
            return
        self._background_task_started = True
        self.sio.start_background_task(self._emit_metrics_loop)

    async def _emit_metrics_loop(self) -> None:
        while True:
            await self.emit_snapshot()
            await self.sio.sleep(METRICS_INTERVAL_SECONDS)
