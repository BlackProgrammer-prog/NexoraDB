"""Bounded asynchronous Socket.IO monitoring with authenticated health fallback."""

from __future__ import annotations

import asyncio
import logging
import math
import time
from dataclasses import dataclass, field
from typing import Callable
from urllib.parse import urlsplit

import aiohttp
import socketio

from .query_client import QueryClient, QueryClientError, strict_json
from .query_editor import safe_text

MAX_MESSAGE = 256 * 1024
MAX_CONNECTIONS = 200
_quiet = logging.getLogger("nexoradb.cli.monitor.transport")
_quiet.addHandler(logging.NullHandler())
_quiet.propagate = False


@dataclass
class ConnectionInfo:
    id: str
    kind: str
    user: str | None
    address: str | None


@dataclass
class MonitoringMetrics:
    database_healthy: bool = False
    requests_per_second: int = 0
    active_connections: list[ConnectionInfo] = field(default_factory=list)
    ram_usage_bytes: int = 0
    disk_usage_bytes: int = 0
    collection_count: int = 0
    connection_count: int = 0
    source: str = "live"


class MonitoringClient:
    def __init__(
        self,
        base_url: str,
        token: str,
        on_metrics: Callable,
        on_status: Callable,
        on_auth_error=lambda: None,
        *,
        http=None,
    ):
        self.base_url, self.token = base_url.rstrip("/"), token
        self.on_metrics, self.on_status, self.on_auth_error = on_metrics, on_status, on_auth_error
        self.http = http
        self._sio = None
        self._stop = asyncio.Event()
        self._disconnected = asyncio.Event()
        self._latest = None
        self._denied = False
        self._last_refresh = 0

    async def connect(self):
        if self.http is None:
            self.http = QueryClient(self.base_url, self.token)
        try:
            attempts = 0
            while not self._stop.is_set():
                try:
                    await self._connect_socketio()
                    attempts = 0
                except asyncio.CancelledError:
                    raise
                except Exception:
                    self.on_status(
                        "Live monitoring unavailable; authenticated health-only fallback"
                    )
                if self._denied:
                    self.on_auth_error()
                    return
                if self._stop.is_set():
                    return
                # Fallback reports missing resource metrics explicitly, never fabricated zeroes.
                await self._poll_once()
                attempts = min(attempts + 1, 5)
                try:
                    await asyncio.wait_for(self._stop.wait(), min(30, 2**attempts))
                except asyncio.TimeoutError:
                    pass
        finally:
            await self.disconnect()
            if self.http:
                await self.http.close()
            self.token = ""
            self._latest = None

    async def disconnect(self):
        self._stop.set()
        if self._sio is not None:
            try:
                await asyncio.wait_for(self._sio.disconnect(), 2)
            except Exception:
                pass
            try:
                await self._sio.shutdown()
            except Exception:
                pass
            self._sio = None

    async def refresh(self):
        now = time.monotonic()
        if now - self._last_refresh < 1:
            return
        self._last_refresh = now
        if self._sio and self._sio.connected:
            await self._sio.emit("metrics:refresh")

    async def _connect_socketio(self):
        async def reject_redirect(session, context, params):
            raise QueryClientError("Monitoring redirects are disabled")

        trace = aiohttp.TraceConfig()
        trace.on_request_redirect.append(reject_redirect)
        async with aiohttp.ClientSession(
            cookie_jar=aiohttp.DummyCookieJar(),
            trust_env=False,
            timeout=aiohttp.ClientTimeout(total=10, sock_connect=5),
            connector=aiohttp.TCPConnector(limit=2),
            trace_configs=[trace],
            auto_decompress=False,
        ) as session:
            sio = socketio.AsyncClient(
                reconnection=False,
                handle_sigint=False,
                http_session=session,
                request_timeout=5,
                websocket_extra_options={"max_msg_size": MAX_MESSAGE, "compress": 0},
                json=_StrictJSON,
                serializer=_TextOnlyPacket,
                logger=_quiet,
                engineio_logger=_quiet,
            )
            self._sio = sio
            self._disconnected.clear()

            @sio.event
            async def connect():
                self.on_status("Live monitoring connected")

            @sio.event
            async def disconnect(*args):
                self._latest = None
                self._disconnected.set()
                if not self._stop.is_set():
                    self.on_status("Monitoring disconnected; metrics unavailable")

            @sio.event
            async def connect_error(data):
                # Do not print a server-controlled error body or credential.
                if isinstance(data, dict) and data.get("message") == "invalid monitoring token":
                    self._denied = True

            @sio.on("metrics")
            async def metrics(data):
                # Coalesce before parsing/formatting connections; one pending snapshot.
                if not self._stop.is_set() and isinstance(data, dict):
                    self._latest = data

            try:
                prefix = urlsplit(self.base_url).path.strip("/")
                await sio.connect(
                    self.base_url,
                    auth={"token": self.token},
                    socketio_path=(prefix + "/socket.io") if prefix else "socket.io",
                    transports=["websocket"],
                    wait_timeout=5,
                )
                last_heartbeat = 0
                while not self._stop.is_set() and not self._disconnected.is_set():
                    data, self._latest = self._latest, None
                    if data is not None:
                        try:
                            self.on_metrics(self._parse_metrics(data))
                        except (ValueError, TypeError):
                            self.on_status("Invalid monitoring snapshot ignored")
                    now = time.monotonic()
                    if now - last_heartbeat >= 5:
                        await sio.emit("heartbeat")
                        last_heartbeat = now
                    await asyncio.sleep(0.25)
            finally:
                try:
                    await asyncio.wait_for(sio.disconnect(), 2)
                except Exception:
                    pass
                await sio.shutdown()
                self._sio = None

    async def _poll_once(self):
        try:
            profile = await self.http.request("GET", "/auth/me", timeout=5)
            if profile.get("role") != "admin" or profile.get("status", "active") != "active":
                self._denied = True
                self.on_auth_error()
                self._stop.set()
                return
            await self.http.request("GET", "/health", timeout=5)
            self.on_metrics(MonitoringMetrics(database_healthy=True, source="health"))
        except QueryClientError as exc:
            if exc.status in {401, 403}:
                self._denied = True
                self.on_auth_error()
                self._stop.set()
                return
            self.on_metrics(MonitoringMetrics(source="health"))

    @staticmethod
    def _parse_metrics(data):
        if not isinstance(data, dict) or type(data.get("databaseHealthy")) is not bool:
            raise ValueError("Invalid monitoring health field")

        def number(key, fallback=None):
            value = data.get(key, data.get(fallback, 0) if fallback else 0)
            if (
                type(value) not in (int, float)
                or not 0 <= value <= 2**63 - 1
                or not math.isfinite(value)
            ):
                raise ValueError("Invalid monitoring numeric field")
            return int(value)

        raw = data.get("activeConnections", [])
        if not isinstance(raw, list):
            raise ValueError("Invalid connections list")
        connections, seen = [], set()
        for item in raw[:MAX_CONNECTIONS]:
            if not isinstance(item, dict):
                raise ValueError("Invalid connection")
            identifier = item.get("id")
            if not isinstance(identifier, str) or not 1 <= len(identifier) <= 128:
                raise ValueError("Invalid connection identifier")
            if identifier in seen:
                continue
            seen.add(identifier)

            def text(key, item=item):
                value = item.get(key)
                if value is not None and not isinstance(value, str):
                    raise ValueError("Invalid connection field")
                return safe_text(value, 256) if value is not None else None

            connections.append(
                ConnectionInfo(identifier, text("kind") or "http", text("user"), text("address"))
            )
        return MonitoringMetrics(
            data["databaseHealthy"],
            number("requestsPerSecond"),
            connections,
            number("ramUsedBytes", "ramUsageBytes"),
            number("ssdUsedBytes", "diskUsageBytes"),
            number("collectionCount"),
            len(raw),
        )


class _StrictJSON:
    import json as _json

    dumps = staticmethod(_json.dumps)
    loads = staticmethod(strict_json)


class _TextOnlyPacket(socketio.packet.Packet):
    def decode(self, encoded_packet):
        count = super().decode(encoded_packet)
        if count or self.packet_type in {socketio.packet.BINARY_EVENT, socketio.packet.BINARY_ACK}:
            raise ValueError("Binary monitoring attachments are unsupported")
        return count
