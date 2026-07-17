"""
NexoraDB monitoring client (socket.io).
مسیر: src/nexoradb/cli/monitoring.py

با MonitoringSocketServer در nexoradb_admin/monitoring.py هماهنگ است.
رویدادهای server: emit("metrics", {...})
رویدادهای client: send("heartbeat"), send("metrics:refresh")
"""
from __future__ import annotations

import json
import threading
import time
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any


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


class MonitoringClient:
    """
    Socket.IO client برای دریافت metrics از سرور.
    از کتابخانه python-socketio (که در dependencies پروژه هست) استفاده می‌کند.
    اگر socket.io در دسترس نبود، polling HTTP ساده را جایگزین می‌کند.
    """

    def __init__(
        self,
        base_url: str,
        token: str,
        on_metrics: Callable[[MonitoringMetrics], None],
        on_status: Callable[[str], None],
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.on_metrics = on_metrics
        self.on_status = on_status
        self._sio: Any = None
        self._connected = False
        self._stop_event = threading.Event()

    # ── public ──────────────────────────────────────────────────────────────

    def connect(self) -> None:
        """اتصال به سرور — ابتدا socket.io، fallback به polling."""
        try:
            self._connect_socketio()
        except Exception as exc:  # noqa: BLE001
            self.on_status(f"socket.io unavailable, polling ({exc})")
            self._poll_loop()

    def disconnect(self) -> None:
        self._stop_event.set()
        if self._sio and self._connected:
            try:
                self._sio.disconnect()
            except Exception:  # noqa: BLE001
                pass

    def heartbeat(self) -> None:
        if self._sio and self._connected:
            try:
                self._sio.emit("heartbeat")
            except Exception:  # noqa: BLE001
                pass

    def refresh(self) -> None:
        if self._sio and self._connected:
            try:
                self._sio.emit("metrics:refresh")
            except Exception:  # noqa: BLE001
                pass

    # ── socket.io ───────────────────────────────────────────────────────────

    def _connect_socketio(self) -> None:
        import socketio  # type: ignore[import]

        sio = socketio.Client(reconnection=True, reconnection_attempts=5)
        self._sio = sio

        @sio.event
        def connect() -> None:
            self._connected = True
            self.on_status("connected")

        @sio.event
        def disconnect() -> None:
            self._connected = False
            self.on_status("disconnected")

        @sio.on("metrics")
        def on_metrics(data: dict[str, Any]) -> None:
            self.on_metrics(self._parse_metrics(data))

        socket_url = self.base_url.replace("http://", "ws://").replace("https://", "wss://")
        sio.connect(
            socket_url,
            auth={"token": self.token},
            socketio_path="/socket.io",
            transports=["websocket", "polling"],
            wait_timeout=10,
        )

        # منتظر بمانیم تا stop_event تنظیم شود
        while not self._stop_event.is_set():
            time.sleep(1)

        sio.disconnect()

    # ── HTTP polling fallback ────────────────────────────────────────────────

    def _poll_loop(self) -> None:
        """هر ۳ ثانیه یه بار /health را poll می‌کند و یه metrics ساده می‌سازد."""
        while not self._stop_event.is_set():
            try:
                req = urllib.request.Request(
                    self.base_url + "/health",
                    headers={"Authorization": f"Bearer {self.token}"},
                )
                with urllib.request.urlopen(req, timeout=5) as resp:
                    healthy = resp.status == 200
                metrics = MonitoringMetrics(database_healthy=healthy)
                self.on_metrics(metrics)
            except Exception:  # noqa: BLE001
                self.on_metrics(MonitoringMetrics(database_healthy=False))
            self._stop_event.wait(timeout=3)

    # ── parser ──────────────────────────────────────────────────────────────

    @staticmethod
    def _parse_metrics(data: dict[str, Any]) -> MonitoringMetrics:
        """
        پیام metrics از server را به MonitoringMetrics تبدیل می‌کند.
        فرمت server (از nexoradb_admin/monitoring.py):
        {
          "databaseHealthy": bool,
          "requestsPerSecond": int,
          "activeConnections": [{"id":..., "kind":..., "user":..., "address":...}],
          "ramUsageBytes": int,
          "diskUsageBytes": int,
        }
        """
        conns = [
            ConnectionInfo(
                id=c.get("id", ""),
                kind=c.get("kind", "http"),
                user=c.get("user"),
                address=c.get("address"),
            )
            for c in data.get("activeConnections", [])
        ]
        return MonitoringMetrics(
            database_healthy=bool(data.get("databaseHealthy", False)),
            requests_per_second=int(data.get("requestsPerSecond", 0)),
            active_connections=conns,
            ram_usage_bytes=int(data.get("ramUsageBytes", 0)),
            disk_usage_bytes=int(data.get("diskUsageBytes", 0)),
        )
