from __future__ import annotations

import argparse
import re
import time
from collections import deque

from textual.actions import SkipAction
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Container, Grid, Horizontal, ScrollableContainer, Vertical
from textual.screen import Screen
from textual.widgets import (
    Button,
    Checkbox,
    DataTable,
    Footer,
    Header,
    Input,
    Label,
    RichLog,
    Select,
    Sparkline,
    Static,
    TabbedContent,
    TabPane,
)

from .api_client import AdminApiClient, AuthSession
from .console_api import ConsoleApi, graph_query
from .monitoring import MonitoringClient, MonitoringMetrics
from .presentation import PrivateInput, SecretInput, json_preview, literal
from .query_client import QueryClientError, strict_json
from .query_workspace import QueryWorkspace
from .tasks import ScreenTasks
from .security_panel import SecurityPanel

DEFAULT_APP_SCOPES = ["query:execute"]
SCOPE_PRESETS: dict[str, list[str]] = {
    "Query only": ["query:execute"],
    "Read only": [
        "query:execute",
        "collections:read",
        "documents:read",
        "graphs:read",
        "monitoring:read",
    ],
    "Read / write": [
        "query:execute",
        "collections:read",
        "collections:write",
        "documents:read",
        "documents:write",
        "graphs:read",
        "graphs:write",
    ],
    "Full access": [
        "query:execute",
        "collections:read",
        "collections:write",
        "documents:read",
        "documents:write",
        "graphs:read",
        "graphs:write",
        "monitoring:read",
        "admin:apps",
    ],
}

# ── الگوریتم‌های گراف — ۱۲ دکمه داشبورد ادمین ─────────────────────────────
# هر آیتم: (label نمایشی, algo_id که به API پاس می‌شود, نوع: lock | job)
GRAPH_ALGORITHMS: list[tuple[str, str, str]] = [
    # ─ LockAlgorithms (سبک — <100ms) ─────────────────────────────────────
    ("۱ · Get Friends", "GetFriends", "lock"),
    ("۲ · Are Connected", "AreConnected", "lock"),
    ("۳ · Shortest Path", "ShortestPath", "lock"),
    ("۴ · Friend Suggestion", "FriendSuggestion", "lock"),
    ("۶ · Most Connected", "MostConnected", "lock"),
    ("۷ · Mutual Friends", "MutualFriends", "lock"),
    ("۸ · Network Stats", "NetworkStats", "lock"),
    # ─ JobAlgorithms (سنگین — background thread) ─────────────────────────
    ("۵ · Components (DSU)", "ConnectedComponents", "job"),
    ("۹ · All Distances", "AllDistances", "job"),
    # ─ Bonus ─────────────────────────────────────────────────────────────
    ("B1 · Betweenness", "BetweennessCentrality", "bonus"),
    ("B2 · Community", "CommunityDetection", "bonus"),
    ("B3 · Influence Max", "InfluenceMaximization", "bonus"),
]

NEXORADB_BANNER = """\
███╗   ██╗███████╗██╗  ██╗ ██████╗ ██████╗  █████╗ ██████╗ ██████╗
████╗  ██║██╔════╝╚██╗██╔╝██╔═══██╗██╔══██╗██╔══██╗██╔══██╗██╔══██╗
██╔██╗ ██║█████╗   ╚███╔╝ ██║   ██║██████╔╝███████║██║  ██║██████╔╝
██║╚██╗██║██╔══╝   ██╔██╗ ██║   ██║██╔══██╗██╔══██║██║  ██║██╔══██╗
██║ ╚████║███████╗██╔╝ ██╗╚██████╔╝██║  ██║██║  ██║██████╔╝██████╔╝
╚═╝  ╚═══╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝╚═════╝ ╚═════╝\
"""


# ══════════════════════════════════════════════════════════════════════════════
# LoginScreen  (بدون تغییر — فقط CSS ریسپانسیو‌تر شد)
# ══════════════════════════════════════════════════════════════════════════════


class LoginScreen(ScreenTasks, Screen[AuthSession]):
    CSS = """
    LoginScreen {
        align: center middle;
        background: $surface;
    }

    #auth-card {
        width: auto;
        max-width: 95%;
        min-width: 30;
        height: auto;
        border: tall $accent;
        padding: 1 2;
        background: $panel;
    }

    .banner {
        width: auto;
        color: $accent;
        text-style: bold;
        margin-bottom: 1;
        overflow: hidden;
    }

    .hero {
        text-style: bold;
        color: $text;
        text-align: center;
        margin-bottom: 1;
    }

    .muted {
        color: $text-muted;
        text-align: center;
        margin-bottom: 1;
    }

    Input {
        margin-bottom: 1;
        width: 100%;
    }

    #auth-error {
        color: $error;
        min-height: 1;
    }
    """

    BINDINGS = [
        ("ctrl+j", "submit", "Submit"),
        ("tab", "focus_next", "Next field"),
        ("shift+tab", "focus_previous", "Previous field"),
    ]

    def __init__(self, client: AdminApiClient) -> None:
        super().__init__()
        self.client = client
        self._init_tasks()
        self.async_api = None

    def compose(self) -> ComposeResult:
        with Container(id="auth-card"):
            yield Static(NEXORADB_BANNER, classes="banner")
            yield Static("CLI Console", classes="hero")
            yield Static("Sign in with your enrolled username. Owner enrollment is local to the database host.", classes="muted")
            with TabbedContent(id="auth-tabs"):
                with TabPane("Login", id="login-tab"):
                    yield Input(placeholder="Username", id="login-username", max_length=254)
                    yield PrivateInput(
                        placeholder="Password", password=True, id="login-password", max_length=1024
                    )
                    yield Button("Login (ctrl+j)", id="login-button", variant="primary")
                with TabPane("Local setup", id="register-tab"):
                    yield Static("On the database host: nexora_iam init <path>. Existing data: nexora_iam adopt <path>. Save recovery factors offline.")
                    yield Button("Local setup only", id="register-button", disabled=True)
                with TabPane("Password reset", id="reset-tab"):
                    yield PrivateInput(placeholder="Administrator-issued reset token", password=True, id="reset-token", max_length=72)
                    yield PrivateInput(placeholder="New password (15+ characters)", password=True, id="reset-password", max_length=1024)
                    yield Button("Complete password reset", id="reset-button")
            yield Static("", id="auth-error")
        yield Footer()

    def on_mount(self) -> None:
        self.query_one("#login-username", Input).focus()
        self.async_api = ConsoleApi(self.client.base_url)
        self._launch("setup", self._load_setup_state)

    async def on_unmount(self):
        for field in ("login-password", "register-password"):
            for widget in self.query("#" + field):
                widget.value = ""
        await self._close_tasks()
        if self.async_api:
            await self.async_api.close()

    def _busy_changed(self, name, busy):
        if name == "auth":
            for identifier in ("login-button", "register-button"):
                    self.query_one("#" + identifier, Button).disabled = busy or identifier == "register-button"

    def _task_error(self, name, error):
        self._set_error(str(error))

    def action_submit(self) -> None:
        if self.query_one("#auth-tabs", TabbedContent).active == "register-tab":
            self._register()
        else:
            self._login()

    async def _load_setup_state(self):
        state = await self.async_api.setup_state()
        if state.get("needsSetup") and not self._closing:
            self._activate_register()

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "login-button":
            self._login()
        elif event.button.id == "register-button":
            self._register()
        elif event.button.id == "reset-button":
            token = self.query_one("#reset-token", Input).value
            password = self.query_one("#reset-password", Input).value
            self.query_one("#reset-token", Input).value = ""
            self.query_one("#reset-password", Input).value = ""
            async def reset():
                await self.async_api.client.request("POST", "/auth/reset-password", {"resetToken": token, "newPassword": password})
                self._set_error("Password reset complete; sign in with the new password")
            self._launch("auth", reset)

    def _login(self) -> None:
        if "auth" in self._tasks or self._closing or self._leaving:
            return
        username = self.query_one("#login-username", Input).value.strip()
        password = self.query_one("#login-password", Input).value
        self.query_one("#login-password", Input).value = ""
        self._set_error("Signing in...")
        self._launch("auth", lambda: self._login_worker(username, password))

    async def _login_worker(self, username, password):
        session = await self.async_api.login(username, password)
        if not self._closing and not self._leaving:
            self._leaving = True
            self.dismiss(session)

    def _register(self) -> None:
        self._set_error("Owner enrollment is offline only. Ask the database operator to run nexora_iam init/adopt.")

    def _set_error(self, message):
        if not self._closing and not self._leaving:
            self.query_one("#auth-error", Static).update(literal(message))

    def _activate_register(self):
        self.query_one("#auth-tabs", TabbedContent).active = "register-tab"


class MainScreen(ScreenTasks, Screen[None]):
    CSS = """
    /* ── کلی ──────────────────────────────────────────────────────────── */
    MainScreen {
        background: $surface;
    }

    #shell {
        height: 1fr;
        padding: 0 1;
    }

    #title-bar {
        height: 1;
        padding: 0 1;
        color: $accent;
        text-style: bold;
    }

    .card {
        border: round $primary;
        padding: 1 2;
        background: $panel;
    }

    .title {
        text-style: bold;
        color: $accent;
        height: 1;
        padding: 0 1;
    }

    .muted {
        color: $text-muted;
    }

    /* ── Monitor ───────────────────────────────────────────────────────── */
    .metric {
        width: 1fr;
        height: 5;
        min-height: 5;
        max-height: 5;
        padding: 0 1;
        content-align: center middle;
    }

    #metrics-grid {
        grid-size: 6;
        grid-columns: 1fr 1fr 1fr 1fr 1fr 1fr;
        grid-rows: 5;
        grid-gutter: 1;
        height: auto;
        margin-bottom: 1;
    }

    #rps-chart {
        height: 5;
        color: $success;
        margin-bottom: 1;
    }

    #connections-table {
        height: 12;
    }

    #monitor-scroll {
        height: 1fr;
    }

    /* ── App Tokens ────────────────────────────────────────────────────── */
    #token-form {
        height: auto;
        margin-bottom: 1;
    }

    #token-form Input {
        margin-bottom: 1;
        width: 100%;
    }

    #scope-presets {
        height: 3;
        margin-bottom: 1;
        overflow-x: auto;
    }

    #scope-presets Button {
        margin-right: 1;
        min-width: 14;
        max-width: 20;
    }

    #scope-list {
        height: auto;
        max-height: 8;
        border: round $primary;
        padding: 0 1;
        margin-bottom: 1;
        overflow-y: auto;
    }

    #token-output {
        height: 10;
        border: round $primary;
    }

    #tokens-scroll {
        height: 1fr;
    }

    /* ── Admin Dashboard ───────────────────────────────────────────────── */
    #algorithms-scroll {
        height: 1fr;
    }

    #graph-selector-row {
        height: 5;
        margin-bottom: 1;
        align: left middle;
    }

    #graph-select-label {
        width: auto;
        min-width: 16;
        padding: 1 1;
        color: $text;
    }

    #graph-select {
        width: 30;
        margin-right: 2;
    }

    #graph-param-label {
        width: auto;
        min-width: 12;
        padding: 1 1;
        color: $text-muted;
    }

    #graph-param-input {
        width: 1fr;
        max-width: 40;
    }

    #algo-grid {
        grid-size: 4;
        grid-columns: 1fr 1fr 1fr 1fr;
        grid-rows: 5;
        grid-gutter: 1;
        height: auto;
        margin-bottom: 1;
    }

    /* دکمه‌های LockAlgorithm */
    .algo-btn-lock {
        width: 100%;
        background: $panel;
        border: tall $success;
        color: $success;
        text-style: bold;
    }

    .algo-btn-lock:hover {
        background: $success 20%;
    }

    /* دکمه‌های JobAlgorithm */
    .algo-btn-job {
        width: 100%;
        background: $panel;
        border: tall $warning;
        color: $warning;
        text-style: bold;
    }

    .algo-btn-job:hover {
        background: $warning 20%;
    }

    /* دکمه‌های Bonus */
    .algo-btn-bonus {
        width: 100%;
        background: $panel;
        border: tall $accent;
        color: $accent;
        text-style: bold;
    }

    .algo-btn-bonus:hover {
        background: $accent 20%;
    }

    #algo-legend {
        height: 2;
        margin-bottom: 1;
        padding: 0 1;
    }

    #algo-legend Label {
        width: auto;
        margin-right: 3;
        color: $text-muted;
    }

    #algo-output {
        height: 12;
        border: round $primary;
        margin-bottom: 1;
    }

    #algo-status {
        height: 1;
        padding: 0 1;
        color: $text-muted;
    }

    .medium #graph-selector-row {
            height: auto;
            layout: vertical;
            align: left top;
    }

    .medium #graph-select-label, .medium #graph-param-label {
            min-width: 0;
            width: 100%;
            padding: 0 1;
    }

    .medium #graph-select, .medium #graph-param-input {
            width: 100%;
            max-width: 100%;
            margin: 0 0 1 0;
    }

    .medium #algo-grid {
            grid-size: 2;
            grid-columns: 1fr 1fr;
    }

    .medium #algo-legend {
            height: auto;
            layout: vertical;
    }

    .medium #metrics-grid {
            grid-size: 3;
            grid-columns: 1fr 1fr 1fr;
    }

    .narrow #shell {
            padding: 0;
    }

    .narrow #title-bar {
            display: none;
    }

    .narrow #metrics-grid {
            grid-size: 1;
            grid-columns: 1fr;
    }

    .narrow .metric {
            width: 100%;
            height: 5;
            min-height: 5;
            max-height: 5;
            padding: 0 1;
    }

    .narrow #query-actions, .narrow #scope-presets {
            height: auto;
            layout: vertical;
    }

    .narrow #query-actions Button, .narrow #scope-presets Button {
            width: 100%;
            max-width: 100%;
            margin: 0 0 1 0;
    }

    .narrow #algo-grid {
            grid-size: 1;
            grid-columns: 1fr;
    }

    .narrow #algo-output {
            height: 9;
    }
    """

    BINDINGS = [
        ("f1", "show_tab('monitor-tab')", "Monitor"),
        ("f2", "show_tab('query-tab')", "Query"),
        ("f3", "show_tab('tokens-tab')", "Tokens"),
        ("f4", "show_tab('algorithms-tab')", "Graph algorithms"),
        ("f5", "run_or_create", "Run/Create"),
        ("f6", "stop_query", "Stop query"),
        ("f7", "next_query_page", "Next cursor"),
        ("r", "refresh_metrics", "Refresh"),
        ("ctrl+enter", "run_or_create", "Run/Create"),
        ("ctrl+k", "clear_active_output", "Clear"),
        ("ctrl+l", "logout", "Logout"),
        ("q", "app.quit", "Quit"),
    ]

    def __init__(self, client: AdminApiClient, session: AuthSession) -> None:
        super().__init__()
        self.client = client
        self._init_tasks()
        self.async_api = None
        self.session = session
        self.metrics = MonitoringMetrics()
        self.rps_samples = deque(maxlen=60)
        self._pending_metrics = None
        self._pending_status = None
        self._last_metrics_at = 0
        self._last_sample_at = 0
        self._cards = {}
        self._connection_rows = {}
        self._issued_token = None
        self._hide_timer = None
        self._leaving = False
        self.monitor: MonitoringClient | None = None
        self.available_scopes: list[str] = list(DEFAULT_APP_SCOPES)
        self._scope_widgets: dict[str, Checkbox] = {}
        # لیست گراف‌هایی که از API لود می‌شوند
        self._available_graphs: list[str] = []

    # ── compose ────────────────────────────────────────────────────────────

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Vertical(id="shell"):
            yield Static(
                f"NexoraDB CLI Console · signed in as {self.session.username}",
                id="title-bar",
            )
            with TabbedContent(id="main-tabs"):
                with TabPane("Monitor (f1)", id="monitor-tab"):
                    yield from self._monitor_layout()
                with TabPane("Query (f2)", id="query-tab"):
                    yield from self._query_layout()
                with TabPane("App tokens (f3)", id="tokens-tab"):
                    yield from self._tokens_layout()
                with TabPane("Graph algorithms (f4)", id="algorithms-tab"):
                    yield from self._algorithms_layout()
                with TabPane("Account and access", id="security-tab"):
                    yield SecurityPanel(
                        self.client.base_url, self.session.access_token,
                        is_superuser=self.session.is_superuser,
                        protected_root=self.session.protected_root,
                    )
        yield Footer()

    # ── Monitor layout ────────────────────────────────────────────────────

    def _monitor_layout(self) -> ComposeResult:
        with ScrollableContainer(id="monitor-scroll"):
            with Grid(id="metrics-grid"):
                yield Static("Database\nwaiting", id="database-health", classes="card metric")
                yield Static("Traffic\n0 req/s", id="rps-value", classes="card metric")
                yield Static("Connections\n0", id="active-apps", classes="card metric")
                yield Static("RAM\nwaiting", id="ram-value", classes="card metric")
                yield Static("SSD\nwaiting", id="ssd-value", classes="card metric")
                yield Static("Collections\nwaiting", id="collection-value", classes="card metric")
            yield Static("Traffic (last 60s)", classes="title")
            yield Sparkline([], id="rps-chart", classes="card")
            yield Static("Active connections (last 10s)", classes="title")
            table = DataTable(id="connections-table")
            for label, width in (("id", 24), ("kind", 12), ("user/app", 24), ("address", 32)):
                table.add_column(label, key=label, width=width)
            yield table

    # ── Query layout ──────────────────────────────────────────────────────

    def _query_layout(self) -> ComposeResult:
        yield QueryWorkspace(self.client.base_url, self.session.access_token)

    def _tokens_layout(self) -> ComposeResult:
        with ScrollableContainer(id="tokens-scroll"):
            yield Static("Create a token for an external application", classes="title")
            with Vertical(id="token-form"):
                yield Input(placeholder="app id, e.g. billing-service", id="app-id", max_length=128)
                yield Input(placeholder="app display name", id="app-name", max_length=256)
                yield Input(
                    value="86400",
                    placeholder="expires in seconds (empty = one day)",
                    id="app-expiry",
                    max_length=12,
                )
            yield Static("Permission presets", classes="title")
            with Horizontal(id="scope-presets"):
                for preset_name in SCOPE_PRESETS:
                    safe_id = preset_name.lower().replace(" ", "-").replace("/", "-")
                    yield Button(preset_name, classes="scope-preset", id=f"preset-{safe_id}")
            yield Static("Application permissions", classes="title")
            with Vertical(id="scope-list"):
                for scope in DEFAULT_APP_SCOPES:
                    yield Checkbox(scope, id=f"scope-{scope.replace(':', '-')}", value=True)
            yield Input(value="{}", placeholder='Collection grants JSON: {"orders":["read"]}', id="app-collections", max_length=8192)
            yield Button("Create app token (ctrl+enter)", id="create-token", variant="success")
            yield SecretInput(
                "",
                password=True,
                id="issued-token",
                max_length=16384,
                placeholder="New token (hidden until Reveal)",
            )
            with Horizontal():
                yield Button("Reveal token (30s)", id="reveal-token", disabled=True)
                yield Button("Clear token", id="clear-token", disabled=True)
            yield RichLog(
                id="token-output", highlight=False, markup=False, max_lines=200, wrap=True
            )

    # ── Admin Dashboard layout ────────────────────────────────────────────

    def _algorithms_layout(self) -> ComposeResult:
        with ScrollableContainer(id="algorithms-scroll"):
            # ── انتخاب گراف و پارامتر ────────────────────────────────────
            yield Static("Graph Algorithm Dashboard", classes="title")
            with Horizontal(id="graph-selector-row"):
                yield Label("Select graph:", id="graph-select-label")
                yield Select(
                    [],
                    id="graph-select",
                    prompt="— loading graphs —",
                )
                yield Label("Params (JSON array):", id="graph-param-label")
                yield Input(
                    placeholder='e.g. ["u1","u2"]',
                    id="graph-param-input",
                    max_length=4096,
                )

            # ── legend رنگ‌ها ──────────────────────────────────────────
            with Horizontal(id="algo-legend"):
                yield Label("■ LockAlgorithm (<100ms)")
                yield Label("■ JobAlgorithm (background)")
                yield Label("■ Bonus")

            # Grid automatically becomes 4, 2, or 1 columns by terminal width.
            yield Static("Algorithms", classes="title")
            with Grid(id="algo-grid"):
                for label, algo_id, kind in GRAPH_ALGORITHMS:
                    yield Button(
                        label,
                        id=f"algo-{algo_id}",
                        classes=f"algo-btn-{kind}",
                    )

            # ── خروجی الگوریتم ─────────────────────────────────────────
            yield Static("Output", classes="title")
            yield Static("", id="algo-status")
            yield RichLog(id="algo-output", highlight=False, markup=False, max_lines=200, wrap=True)

    # ── on_mount ──────────────────────────────────────────────────────────

    def on_mount(self) -> None:
        self._apply_responsive_classes(self.size.width)
        for scope in DEFAULT_APP_SCOPES:
            self._scope_widgets[scope] = self.query_one(
                "#scope-" + scope.replace(":", "-"), Checkbox
            )
        self.async_api = ConsoleApi(self.client.base_url, self.session.access_token)
        self._start_monitoring()
        self._launch("scopes", self._load_app_scopes)
        self._launch("graphs", self._load_graph_list)
        self.set_interval(0.25, self._flush_metrics)

    def on_resize(self, event):
        self._apply_responsive_classes(event.size.width)

    def _apply_responsive_classes(self, width):
        self.set_class(width < 100, "medium")
        self.set_class(width < 65, "narrow")

    async def on_unmount(self):
        self._clear_token()
        await self._close_tasks()
        if self.monitor:
            await self.monitor.disconnect()
        if self.async_api:
            await self.async_api.close()
        self._issued_token = None
        self.session.access_token = ""
        self._pending_metrics = None
        self._pending_status = None
        self.rps_samples.clear()
        for widget in self.query("#issued-token"):
            widget.value = ""

    def _busy_changed(self, name, busy):
        if name == "token":
            self.query_one("#create-token", Button).disabled = busy
        elif name == "algorithm":
            for _, identifier, _ in GRAPH_ALGORITHMS:
                self.query_one("#algo-" + identifier, Button).disabled = busy

    def _task_error(self, name, error):
        if isinstance(error, QueryClientError) and error.status == 401:
            self.action_logout()
            return
        if name == "monitor":
            self._set_monitor_status("Monitoring unavailable")
        elif name in {"algorithm", "graphs"}:
            self._write_algo_log(str(error))
            self.query_one("#algo-status", Static).update(
                literal("Algorithm failed or unavailable")
            )
        else:
            self._write_token_error(str(error))

    def _start_monitoring(self):
        self.monitor = MonitoringClient(
            self.client.base_url,
            self.session.access_token,
            on_metrics=self._queue_metrics,
            on_status=self._queue_status,
            on_auth_error=self.action_logout,
        )
        self._launch("monitor", self.monitor.connect)

    def _queue_metrics(self, metrics):
        if not self._closing and not self._leaving:
            self._pending_metrics = metrics
            self._last_metrics_at = time.monotonic()

    def _queue_status(self, status):
        if not self._closing and not self._leaving:
            self._pending_status = status

    def _flush_metrics(self):
        if self._closing or self.query_one("#main-tabs", TabbedContent).active != "monitor-tab":
            return
        metrics, self._pending_metrics = self._pending_metrics, None
        if metrics is not None:
            self._update_metrics(metrics)
        if self._pending_status:
            self._set_monitor_status(self._pending_status)
            self._pending_status = None
        if self._last_metrics_at and time.monotonic() - self._last_metrics_at > 15:
            self._set_monitor_status("Monitoring stale; current health unknown")

    def action_refresh_metrics(self):
        if self.monitor:
            self._launch("refresh", self.monitor.refresh)

    async def _load_graph_list(self):
        graphs = await self.async_api.graphs()
        if not self._closing and not self._leaving:
            self._available_graphs = graphs
            self._populate_graph_select(graphs)

    def _populate_graph_select(self, graphs):
        self.query_one("#graph-select", Select).set_options(
            [(literal(g, 128), g) for g in graphs] or [("(no graphs found)", "")]
        )

    def action_logout(self):
        if not self._leaving and not self._closing:
            self._clear_token()
            for workspace in self.query(QueryWorkspace):
                workspace.disabled = True
                workspace.action_stop_query()
            async def leave():
                try:
                    await self.async_api.client.request("POST", "/auth/logout", {})
                except QueryClientError:
                    pass
                finally:
                    if not self._closing:
                        self._leaving = True
                        self.dismiss(None)
            self._launch("logout", leave)

    def on_query_workspace_session_expired(self, event):
        event.stop()
        self.action_logout()

    def on_security_panel_password_changed(self, event):
        event.stop()
        self.action_logout()

    def action_show_tab(self, tab_id):
        self.set_focus(None)
        self.query_one("#main-tabs", TabbedContent).active = tab_id
        if tab_id != "tokens-tab":
            self._hide_token()
        target = {
            "monitor-tab": "#monitor-scroll",
            "query-tab": "#query-editor",
            "tokens-tab": "#app-id",
            "algorithms-tab": "#graph-select",
        }.get(tab_id)
        if target:
            self.query_one(target).focus()

    def on_tabbed_content_tab_activated(self, event):
        if event.tabbed_content.id == "main-tabs" and event.tab is not event.tabbed_content.get_tab(
            "tokens-tab"
        ):
            self._hide_token()

    def action_run_or_create(self):
        active = self.query_one("#main-tabs", TabbedContent).active
        if active == "query-tab":
            self._run_query()
        elif active == "tokens-tab":
            self._create_app_token()

    def action_stop_query(self):
        if self.query_one("#main-tabs", TabbedContent).active == "query-tab":
            self.query_one(QueryWorkspace).action_stop_query()

    def action_next_query_page(self):
        if self.query_one("#main-tabs", TabbedContent).active == "query-tab":
            self.query_one(QueryWorkspace).action_next_cursor()

    def action_clear_active_output(self):
        active = self.query_one("#main-tabs", TabbedContent).active
        if active == "query-tab":
            self.query_one(QueryWorkspace).clear_output()
        elif active == "tokens-tab":
            self._clear_token()
        elif active == "algorithms-tab":
            self.query_one("#algo-output", RichLog).clear()

    def on_button_pressed(self, event):
        bid = event.button.id or ""
        if bid == "create-token":
            self._create_app_token()
        elif bid == "reveal-token":
            self._reveal_token()
        elif bid == "clear-token":
            self._clear_token()
        elif bid.startswith("preset-"):
            for name in SCOPE_PRESETS:
                if name.lower().replace(" ", "-").replace("/", "-") == bid.removeprefix("preset-"):
                    self._apply_scope_preset(name)
        elif bid.startswith("algo-"):
            self._run_algorithm(bid.removeprefix("algo-"))

    def _run_algorithm(self, algo_id):
        if "algorithm" in self._tasks or self._closing:
            return
        value = self.query_one("#graph-select", Select).value
        graph = value if isinstance(value, str) else ""
        try:
            raw = self.query_one("#graph-param-input", Input).value.strip()
            params = strict_json(raw) if raw else []
            graph_query(algo_id, graph, params)
        except (ValueError, RecursionError) as exc:
            self._write_algo_log(str(exc))
            return
        self._write_algo_log("Running " + algo_id + " on " + graph)
        self.query_one("#algo-status", Static).update(literal("Running " + algo_id))
        self._launch("algorithm", lambda: self._algo_worker(algo_id, graph, params))

    async def _algo_worker(self, algo, graph, params):
        result = await self.async_api.algorithm(algo, graph, params)
        if not self._closing and not self._leaving:
            self._render_algo_result(algo, result)

    def _render_algo_result(self, algo, result):
        self._write_algo_log(f"Completed {algo} in {result.get('executionTimeMs', '?')} ms")
        self.query_one("#algo-output", RichLog).write(json_preview(result))
        self.query_one("#algo-status", Static).update(literal("Completed " + algo))

    def _write_algo_log(self, message):
        if not self._closing and not self._leaving:
            self.query_one("#algo-output", RichLog).write(literal(message))

    def _run_query(self):
        self.query_one(QueryWorkspace).action_run_query()

    async def _load_app_scopes(self):
        scopes = await self.async_api.scopes()
        if scopes and not self._closing:
            await self._render_scope_checkboxes(scopes)

    async def _render_scope_checkboxes(self, scopes):
        self.available_scopes = scopes
        self._scope_widgets = {}
        container = self.query_one("#scope-list", Vertical)
        await container.remove_children()
        if self._closing:
            return
        checkboxes = []
        for scope in scopes:
            checkbox = Checkbox(
                literal(scope),
                id="scope-" + scope.replace(":", "-"),
                value=scope in DEFAULT_APP_SCOPES,
            )
            self._scope_widgets[scope] = checkbox
            checkboxes.append(checkbox)
        await container.mount(*checkboxes)

    def _selected_scopes(self):
        return [scope for scope, cb in self._scope_widgets.items() if cb.value]

    def _apply_scope_preset(self, name):
        for scope, cb in self._scope_widgets.items():
            cb.value = scope in SCOPE_PRESETS.get(name, [])

    def _create_app_token(self):
        if "token" in self._tasks or self._closing:
            return
        identifier = self.query_one("#app-id", Input).value.strip()
        name = self.query_one("#app-name", Input).value.strip() or None
        raw = self.query_one("#app-expiry", Input).value.strip()
        scopes = self._selected_scopes()
        if not scopes:
            self._write_token_error("Select at least one permission scope")
            return
        try:
            expiry = int(raw) if raw else None
            grants = strict_json(self.query_one("#app-collections", Input).value)
            if not isinstance(grants, dict):
                raise ValueError()
            if expiry is not None and not 300 <= expiry <= 31536000:
                raise ValueError()
        except ValueError:
            self._write_token_error("Expiry must be 300..31536000 seconds, or empty; collection grants must be a JSON object")
            return
        self._clear_token()
        self._launch("token", lambda: self._token_worker(identifier, name, expiry, scopes, grants))

    async def _token_worker(self, identifier, name, expiry, scopes, grants=None):
        result = await self.async_api.create_token(identifier, name, expiry, scopes, grants)
        if not self._closing and not self._leaving:
            self._write_token(result)

    def _write_token_error(self, message):
        if not self._closing and not self._leaving:
            self.query_one("#token-output", RichLog).write(literal(message, style="red"))

    def _write_token(self, token):
        secret = token.get("token")
        if not isinstance(secret, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,16384}", secret):
            raise QueryClientError("Invalid token response; token was not displayed")
        self._issued_token = secret
        field = self.query_one("#issued-token", Input)
        field.value, field.password = secret, True
        self.query_one("#reveal-token", Button).disabled = False
        self.query_one("#clear-token", Button).disabled = False
        metadata = {
            key: token[key]
            for key in ("tokenId", "appId", "appName", "expiresAt", "scopes")
            if key in token
        }
        self.query_one("#token-output", RichLog).clear()
        self.query_one("#token-output", RichLog).write(json_preview(metadata))

    def _reveal_token(self):
        if self._issued_token:
            self.query_one("#issued-token", Input).password = False
            if self._hide_timer:
                self._hide_timer.stop()
            self._hide_timer = self.set_timer(30, self._hide_token)

    def _hide_token(self):
        if self._hide_timer:
            self._hide_timer.stop()
            self._hide_timer = None
        for field in self.query("#issued-token"):
            field.password = True

    def _clear_token(self):
        self._hide_token()
        self._issued_token = None
        for field in self.query("#issued-token"):
            field.value = ""
        for identifier in ("reveal-token", "clear-token"):
            for button in self.query("#" + identifier):
                button.disabled = True
        for log in self.query("#token-output"):
            log.clear()

    def _card(self, identifier, text):
        if self._cards.get(identifier) != text:
            self.query_one("#" + identifier, Static).update(literal(text, 512))
            self._cards[identifier] = text

    def _update_metrics(self, metrics):
        self.metrics = metrics
        self._card(
            "database-health",
            "Database: " + ("healthy" if metrics.database_healthy else "unhealthy"),
        )
        if metrics.source != "live":
            self._card(
                "database-health",
                "HTTP reachable; database health unknown"
                if metrics.database_healthy
                else "HTTP unavailable; database health unknown",
            )
            self.rps_samples.clear()
            self.query_one("#rps-chart", Sparkline).data = []
            for identifier in (
                "rps-value",
                "active-apps",
                "ram-value",
                "ssd-value",
                "collection-value",
            ):
                self._card(identifier, "Unavailable (health-only fallback)")
            self._sync_connections([])
            return
        now = time.monotonic()
        if now - self._last_sample_at > 2:
            self.rps_samples.clear()
        if now - self._last_sample_at >= 1:
            self.rps_samples.append(metrics.requests_per_second)
            self._last_sample_at = now
            self.query_one("#rps-chart", Sparkline).data = list(self.rps_samples)
        self._card("rps-value", f"Traffic: {metrics.requests_per_second} req/s")
        self._card(
            "active-apps",
            f"Connections: {len(metrics.active_connections)} shown / "
            f"{metrics.connection_count or len(metrics.active_connections)} total",
        )
        self._card("ram-value", "RAM: " + _format_bytes(metrics.ram_usage_bytes))
        self._card("ssd-value", "SSD: " + _format_bytes(metrics.disk_usage_bytes))
        self._card("collection-value", f"Collections: {metrics.collection_count}")
        self._sync_connections(metrics.active_connections)

    def _sync_connections(self, connections):
        table = self.query_one("#connections-table", DataTable)
        rows = {c.id: (c.id, c.kind, c.user or "", c.address or "") for c in connections[:200]}
        for key in self._connection_rows.keys() - rows.keys():
            table.remove_row(key)
        labels = ("id", "kind", "user/app", "address")
        for key, values in rows.items():
            if key not in self._connection_rows:
                table.add_row(*(literal(value, 256) for value in values), key=key)
            elif self._connection_rows[key] != values:
                for label, old, new in zip(labels, self._connection_rows[key], values, strict=True):
                    if old != new:
                        table.update_cell(key, label, literal(new, 256))
        self._connection_rows = rows

    def _set_monitor_status(self, status):
        if self._closing or self._leaving:
            return
        self._card("database-health", status)
        if any(
            word in status.lower() for word in ("unavailable", "disconnected", "stale", "fallback")
        ):
            for identifier in (
                "rps-value",
                "active-apps",
                "ram-value",
                "ssd-value",
                "collection-value",
            ):
                self._card(identifier, "Unavailable (monitoring data stale)")
            self.rps_samples.clear()
            self.query_one("#rps-chart", Sparkline).data = []
            self._sync_connections([])


def _format_bytes(value: int) -> str:
    amount = float(max(0, value))
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if amount < 1024 or unit == "TiB":
            return f"{amount:.0f} {unit}" if unit == "B" else f"{amount:.1f} {unit}"
        amount /= 1024
    return "0 B"


# ══════════════════════════════════════════════════════════════════════════════
# App entry‑point
# ══════════════════════════════════════════════════════════════════════════════


class NexoraCliApp(App[None]):
    TITLE = "NexoraDB CLI"
    SUB_TITLE = "Admin console, monitor, and query workbench"
    BINDINGS = [
        Binding("ctrl+c", "copy_or_quit", show=False, priority=True),
        Binding("ctrl+q", "quit", "Quit", priority=True),
    ]

    def action_copy_or_quit(self):
        # Older Textual handles Ctrl+C at App priority before child bindings.
        # Route it explicitly so masked fields cannot export credentials.
        widget = self.focused
        action = getattr(widget, "action_copy", None)
        if action is not None:
            try:
                action()
            except SkipAction:
                pass
        elif isinstance(getattr(widget, "selected_text", None), str):
            if widget.selected_text:
                self.copy_to_clipboard(widget.selected_text)
        else:
            self.exit()

    def __init__(self, base_url: str) -> None:
        super().__init__()
        self.client = AdminApiClient(base_url)

    def on_mount(self) -> None:
        self._show_login()

    def _show_login(self) -> None:
        self.push_screen(LoginScreen(self.client), self._show_main)

    def _show_main(self, session: AuthSession | None) -> None:
        if session is None:
            self._show_login()
            return
        self.push_screen(MainScreen(self.client, session), lambda _: self._show_login())


def main() -> None:
    parser = argparse.ArgumentParser(description="NexoraDB Textual CLI")
    parser.add_argument("--url", default="http://localhost:8000", help="NexoraDB admin API URL")
    args = parser.parse_args()
    NexoraCliApp(args.url).run()


if __name__ == "__main__":
    main()
