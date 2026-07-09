from __future__ import annotations

import json
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from importlib.util import find_spec
from typing import Any

import socketio


@dataclass
class ActiveConnection:
    id: str
    kind: str = ""
    user: str = ""
    address: str = ""


@dataclass
class MonitoringMetrics:
    database_healthy: bool = False
    ram_used_bytes: int = 0
    ssd_used_bytes: int = 0
    ssd_total_bytes: int = 0
    requests_per_second: int = 0
    active_connections: list[ActiveConnection] = field(default_factory=list)
    received_at: int = field(default_factory=lambda: int(time.time() * 1000))


class MonitoringClient:
    def __init__(
        self,
        *,
        base_url: str,
        token: str,
        on_metrics: Callable[[MonitoringMetrics], None],
        on_status: Callable[[str], None],
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.on_metrics = on_metrics
        self.on_status = on_status
        self.sio = socketio.Client(reconnection=True)
        self._configure_handlers()

    def connect(self) -> None:
        self.sio.connect(
            self.base_url,
            auth={"token": self.token},
            socketio_path="socket.io",
            transports=_available_transports(),
        )

    def disconnect(self) -> None:
        if self.sio.connected:
            self.sio.disconnect()

    def refresh(self) -> None:
        if self.sio.connected:
            self.sio.emit("metrics:refresh")

    def heartbeat(self) -> None:
        if self.sio.connected:
            self.sio.emit("heartbeat")

    def _configure_handlers(self) -> None:
        @self.sio.event
        def connect() -> None:
            self.on_status("connected")
            self.refresh()

        @self.sio.event
        def disconnect() -> None:
            self.on_status("disconnected")

        @self.sio.event
        def reconnect() -> None:
            self.on_status("connected")
            self.refresh()

        @self.sio.event
        def reconnect_attempt() -> None:
            self.on_status("reconnecting")

        @self.sio.event
        def connect_error(data: Any) -> None:
            detail = _format_error_detail(data)
            self.on_status(f"connection failed{f': {detail}' if detail else ''}")

        @self.sio.on("metrics")
        def metrics(payload: Any) -> None:
            self.on_metrics(normalize_metrics(payload))


def normalize_metrics(payload: Any) -> MonitoringMetrics:
    data = payload if isinstance(payload, dict) else {}
    connections = data.get("activeConnections")
    return MonitoringMetrics(
        database_healthy=bool(data.get("databaseEngineHealthy") or data.get("databaseHealthy")),
        ram_used_bytes=_int(data.get("ramUsedBytes")),
        ssd_used_bytes=_int(data.get("ssdUsedBytes")),
        ssd_total_bytes=_int(data.get("ssdTotalBytes")),
        requests_per_second=_int(data.get("requestsPerSecond")),
        active_connections=_normalize_connections(connections),
        received_at=_int(data.get("receivedAt")) or int(time.time() * 1000),
    )


def _normalize_connections(value: Any) -> list[ActiveConnection]:
    if not isinstance(value, list):
        return []
    records: list[ActiveConnection] = []
    for index, item in enumerate(value, start=1):
        if not isinstance(item, dict):
            records.append(ActiveConnection(id=str(item or f"connection-{index}")))
            continue
        records.append(
            ActiveConnection(
                id=str(item.get("id") or f"connection-{index}"),
                kind=str(item.get("kind") or ""),
                user=str(item.get("user") or ""),
                address=str(item.get("address") or ""),
            )
        )
    return records


def _int(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _format_error_detail(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        try:
            return json.dumps(value, separators=(",", ":"))
        except (TypeError, ValueError):
            return str(value)
    return ""


def _available_transports() -> list[str]:
    if find_spec("websocket") is None:
        return ["polling"]
    return ["polling", "websocket"]
