from __future__ import annotations

import argparse
import json
import threading
from typing import Any

from textual.app import App, ComposeResult
from textual.containers import Container, Horizontal, ScrollableContainer, Vertical
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
    TabPane,
    TabbedContent,
    TextArea,
)

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
    ("۱ · Get Friends",        "GetFriends",            "lock"),
    ("۲ · Are Connected",      "AreConnected",          "lock"),
    ("۳ · Shortest Path",      "ShortestPath",          "lock"),
    ("۴ · Friend Suggestion",  "FriendSuggestion",      "lock"),
    ("۶ · Most Connected",     "MostConnected",         "lock"),
    ("۷ · Mutual Friends",     "MutualFriends",         "lock"),
    ("۸ · Network Stats",      "NetworkStats",          "lock"),
    # ─ JobAlgorithms (سنگین — background thread) ─────────────────────────
    ("۵ · Components (DSU)",   "ConnectedComponents",   "job"),
    ("۹ · All Distances",      "AllDistances",          "job"),
    # ─ Bonus ─────────────────────────────────────────────────────────────
    ("B1 · Betweenness",       "BetweennessCentrality", "bonus"),
    ("B2 · Community",         "CommunityDetection",    "bonus"),
    ("B3 · Influence Max",     "InfluenceMaximization", "bonus"),
]

from .api_client import AdminApiClient, AuthSession, NexoraAdminClientError
from .monitoring import MonitoringClient, MonitoringMetrics

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

class LoginScreen(Screen[AuthSession]):
    CSS = """
    LoginScreen {
        align: center middle;
        background: $surface;
    }

    #auth-card {
        width: auto;
        max-width: 95%;
        min-width: 50;
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

    def compose(self) -> ComposeResult:
        with Container(id="auth-card"):
            yield Static(NEXORADB_BANNER, classes="banner")
            yield Static("CLI Console", classes="hero")
            yield Static("Login or bootstrap the first root admin account.", classes="muted")
            with TabbedContent(id="auth-tabs"):
                with TabPane("Login", id="login-tab"):
                    yield Input(placeholder="Username", id="login-username")
                    yield Input(placeholder="Password", password=True, id="login-password")
                    yield Button("Login (ctrl+j)", id="login-button", variant="primary")
                with TabPane("Register root", id="register-tab"):
                    yield Input(placeholder="First name", id="register-first-name")
                    yield Input(placeholder="Last name", id="register-last-name")
                    yield Input(placeholder="Email", id="register-email")
                    yield Input(placeholder="Password", password=True, id="register-password")
                    yield Button("Create root admin (ctrl+j)", id="register-button", variant="success")
            yield Static("", id="auth-error")
        yield Footer()

    def on_mount(self) -> None:
        self.query_one("#login-username", Input).focus()
        self.run_worker(self._load_setup_state, thread=True)

    def action_submit(self) -> None:
        active = self.query_one("#auth-tabs", TabbedContent).active
        if active == "register-tab":
            self._register()
        else:
            self._login()

    def _load_setup_state(self) -> None:
        try:
            state = self.client.setup_state()
        except NexoraAdminClientError as exc:
            self.app.call_from_thread(self._set_error, f"Could not reach admin API: {exc}")
            return
        if state.get("needsSetup"):
            self.app.call_from_thread(self._activate_register)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "login-button":
            self._login()
        elif event.button.id == "register-button":
            self._register()

    def _login(self) -> None:
        username = self.query_one("#login-username", Input).value
        password = self.query_one("#login-password", Input).value
        self._set_error("Signing in...")
        self.run_worker(lambda: self._login_worker(username, password), thread=True)

    def _login_worker(self, username: str, password: str) -> None:
        try:
            session = self.client.login(username, password)
        except NexoraAdminClientError as exc:
            self.app.call_from_thread(self._set_error, str(exc))
            return
        self.app.call_from_thread(self.dismiss, session)

    def _register(self) -> None:
        first_name = self.query_one("#register-first-name", Input).value
        last_name  = self.query_one("#register-last-name",  Input).value
        email      = self.query_one("#register-email",      Input).value
        password   = self.query_one("#register-password",   Input).value
        self._set_error("Creating root admin...")
        self.run_worker(
            lambda: self._register_worker(first_name, last_name, email, password),
            thread=True,
        )

    def _register_worker(
        self, first_name: str, last_name: str, email: str, password: str
    ) -> None:
        try:
            self.client.register(
                first_name=first_name, last_name=last_name,
                email=email, password=password,
            )
            session = self.client.login("root", password)
        except NexoraAdminClientError as exc:
            self.app.call_from_thread(self._set_error, str(exc))
            return
        self.app.call_from_thread(self.dismiss, session)

    def _set_error(self, message: str) -> None:
        self.query_one("#auth-error", Static).update(message)

    def _activate_register(self) -> None:
        self.query_one("#auth-tabs", TabbedContent).active = "register-tab"


# ══════════════════════════════════════════════════════════════════════════════
# MainScreen
# ══════════════════════════════════════════════════════════════════════════════

class MainScreen(Screen[None]):
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
        min-height: 4;
        max-height: 4;
    }

    #metrics-row {
        height: 4;
        margin-bottom: 1;
    }

    #rps-chart {
        height: 5;
        color: $success;
        margin-bottom: 1;
    }

    #connections-table {
        height: 1fr;
    }

    /* ── Query ─────────────────────────────────────────────────────────── */
    #query-editor {
        height: 8;
        border: tall $primary;
        margin-bottom: 1;
    }

    #query-actions {
        height: 3;
        margin-bottom: 1;
    }

    #query-actions Button {
        margin-right: 1;
        min-width: 18;
        max-width: 24;
    }

    #query-results {
        height: 1fr;
    }

    #query-table {
        width: 1fr;
        height: 100%;
        border: round $primary;
        margin-right: 1;
    }

    #query-output {
        width: 1fr;
        height: 100%;
        border: round $primary;
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
        height: 1fr;
        border: round $primary;
    }

    /* ── Admin Dashboard ───────────────────────────────────────────────── */
    #admin-scroll {
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

    #algo-grid-section {
        height: auto;
        margin-bottom: 1;
    }

    /* سه ردیف ۴ تایی — grid با Container های افقی */
    .algo-row {
        height: 5;
        margin-bottom: 1;
    }

    /* دکمه‌های LockAlgorithm */
    .algo-btn-lock {
        width: 1fr;
        margin-right: 1;
        min-width: 18;
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
        width: 1fr;
        margin-right: 1;
        min-width: 18;
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
        width: 1fr;
        margin-right: 1;
        min-width: 18;
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
    """

    BINDINGS = [
        ("f1", "show_tab('monitor-tab')",  "Monitor"),
        ("f2", "show_tab('query-tab')",    "Query"),
        ("f3", "show_tab('tokens-tab')",   "Tokens"),
        ("f4", "show_tab('admin-tab')",    "Admin"),
        ("r",  "refresh_metrics",          "Refresh"),
        ("ctrl+enter", "run_or_create",    "Run/Create"),
        ("ctrl+k",     "clear_active_output", "Clear"),
        ("ctrl+l",     "logout",           "Logout"),
        ("q",          "app.quit",         "Quit"),
    ]

    def __init__(self, client: AdminApiClient, session: AuthSession) -> None:
        super().__init__()
        self.client = client
        self.session = session
        self.metrics = MonitoringMetrics()
        self.rps_samples: list[int] = []
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
                with TabPane("Admin (f4)", id="admin-tab"):
                    yield from self._admin_layout()
        yield Footer()

    # ── Monitor layout ────────────────────────────────────────────────────

    def _monitor_layout(self) -> ComposeResult:
        with Vertical():
            with Horizontal(id="metrics-row"):
                yield Static("Database\nwaiting", id="database-health", classes="card metric")
                yield Static("Traffic\n0 req/s",  id="rps-value",       classes="card metric")
                yield Static("Active apps\n0",    id="active-apps",     classes="card metric")
            yield Static("Traffic (last 60s)", classes="title")
            yield Sparkline([], id="rps-chart", classes="card")
            yield Static("Active connections (last 10s)", classes="title")
            table = DataTable(id="connections-table")
            table.add_columns("id", "kind", "user/app", "address")
            yield table

    # ── Query layout ──────────────────────────────────────────────────────

    def _query_layout(self) -> ComposeResult:
        with Vertical():
            yield Static("NexoraQL query editor", classes="title")
            yield TextArea("SHOW COLLECTIONS;", id="query-editor", language="sql")
            with Horizontal(id="query-actions"):
                yield Button("▶ Run (ctrl+enter)", id="run-query", variant="primary")
                yield Button("⌫ Clear (ctrl+k)",  id="clear-query-output")
            with Horizontal(id="query-results"):
                yield DataTable(id="query-table")
                yield RichLog(id="query-output", highlight=True, markup=True, wrap=True)

    # ── Tokens layout ─────────────────────────────────────────────────────

    def _tokens_layout(self) -> ComposeResult:
        with Vertical():
            yield Static("Create a token for an external application", classes="title")
            with Vertical(id="token-form"):
                yield Input(placeholder="app id, e.g. billing-service", id="app-id")
                yield Input(placeholder="app display name",              id="app-name")
                yield Input(placeholder="expires in seconds (empty = no expiry)", id="app-expiry")
            yield Static("Permission presets", classes="title")
            with Horizontal(id="scope-presets"):
                for preset_name in SCOPE_PRESETS:
                    safe_id = preset_name.lower().replace(" ", "-").replace("/", "-")
                    yield Button(preset_name, classes="scope-preset", id=f"preset-{safe_id}")
            yield Static("Application permissions", classes="title")
            with Vertical(id="scope-list"):
                for scope in DEFAULT_APP_SCOPES:
                    yield Checkbox(scope, id=f"scope-{scope.replace(':', '-')}", value=True)
            yield Button("Create app token (ctrl+enter)", id="create-token", variant="success")
            yield RichLog(id="token-output", highlight=True, markup=True)

    # ── Admin Dashboard layout ────────────────────────────────────────────

    def _admin_layout(self) -> ComposeResult:
        with ScrollableContainer(id="admin-scroll"):
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
                )

            # ── legend رنگ‌ها ──────────────────────────────────────────
            with Horizontal(id="algo-legend"):
                yield Label("■ LockAlgorithm (<100ms)")
                yield Label("■ JobAlgorithm (background)")
                yield Label("■ Bonus")

            # ── ۱۲ دکمه در ۳ ردیف ۴ تایی ─────────────────────────────
            yield Static("Algorithms", classes="title")
            with Vertical(id="algo-grid-section"):
                # ردیف ۱: GetFriends | AreConnected | ShortestPath | FriendSuggestion
                with Horizontal(classes="algo-row"):
                    for label, algo_id, kind in GRAPH_ALGORITHMS[:4]:
                        yield Button(
                            label,
                            id=f"algo-{algo_id}",
                            classes=f"algo-btn-{kind}",
                        )
                # ردیف ۲: MostConnected | MutualFriends | NetworkStats | Components
                with Horizontal(classes="algo-row"):
                    for label, algo_id, kind in GRAPH_ALGORITHMS[4:8]:
                        yield Button(
                            label,
                            id=f"algo-{algo_id}",
                            classes=f"algo-btn-{kind}",
                        )
                # ردیف ۳: AllDistances | Betweenness | Community | InfluenceMax
                with Horizontal(classes="algo-row"):
                    for label, algo_id, kind in GRAPH_ALGORITHMS[8:12]:
                        yield Button(
                            label,
                            id=f"algo-{algo_id}",
                            classes=f"algo-btn-{kind}",
                        )

            # ── خروجی الگوریتم ─────────────────────────────────────────
            yield Static("Output", classes="title")
            yield Static("", id="algo-status")
            yield RichLog(id="algo-output", highlight=True, markup=True, wrap=True)

    # ── on_mount ──────────────────────────────────────────────────────────

    def on_mount(self) -> None:
        for scope in DEFAULT_APP_SCOPES:
            self._scope_widgets[scope] = self.query_one(
                f"#scope-{scope.replace(':', '-')}", Checkbox,
            )
        self._start_monitoring()
        self.run_worker(self._load_app_scopes,   thread=True)
        self.run_worker(self._load_graph_list,   thread=True)

    def on_unmount(self) -> None:
        if self.monitor:
            self.monitor.disconnect()

    # ── monitoring ────────────────────────────────────────────────────────

    def _start_monitoring(self) -> None:
        self.monitor = MonitoringClient(
            base_url=self.client.base_url,
            token=self.session.access_token,
            on_metrics=lambda m: self.app.call_from_thread(self._update_metrics, m),
            on_status=lambda s: self.app.call_from_thread(self._set_monitor_status, s),
        )
        threading.Thread(target=self._connect_monitoring, daemon=True).start()
        self.set_interval(5.0, self._heartbeat)

    def _connect_monitoring(self) -> None:
        if not self.monitor:
            return
        try:
            self.monitor.connect()
        except Exception as exc:  # noqa: BLE001
            self.app.call_from_thread(self._set_monitor_status, f"monitor error: {exc}")

    def _heartbeat(self) -> None:
        if self.monitor:
            self.monitor.heartbeat()

    def action_refresh_metrics(self) -> None:
        if self.monitor:
            self.monitor.refresh()

    # ── graph list ────────────────────────────────────────────────────────

    def _load_graph_list(self) -> None:
        """لیست گراف‌ها را از API می‌گیرد و Select را پر می‌کند."""
        try:
            graphs: list[str] = self.client.list_graphs(self.session.access_token)
        except NexoraAdminClientError as exc:
            # اگر API هنوز endpoint ندارد، fallback خالی
            graphs = []
            self.app.call_from_thread(
                self._write_algo_log, f"[yellow]Could not load graph list: {exc}[/yellow]"
            )
        self._available_graphs = graphs
        self.app.call_from_thread(self._populate_graph_select, graphs)

    def _populate_graph_select(self, graphs: list[str]) -> None:
        sel = self.query_one("#graph-select", Select)
        if graphs:
            sel.set_options([(g, g) for g in graphs])
        else:
            sel.set_options([("(no graphs found)", "")])

    # ── actions ───────────────────────────────────────────────────────────

    def action_logout(self) -> None:
        self.dismiss(None)

    def action_show_tab(self, tab_id: str) -> None:
        self.query_one("#main-tabs", TabbedContent).active = tab_id

    def action_run_or_create(self) -> None:
        active = self.query_one("#main-tabs", TabbedContent).active
        if active == "query-tab":
            self._run_query()
        elif active == "tokens-tab":
            self._create_app_token()

    def action_clear_active_output(self) -> None:
        active = self.query_one("#main-tabs", TabbedContent).active
        if active == "query-tab":
            self.query_one("#query-output", RichLog).clear()
            self.query_one("#query-table", DataTable).clear(columns=True)
        elif active == "tokens-tab":
            self.query_one("#token-output", RichLog).clear()
        elif active == "admin-tab":
            self.query_one("#algo-output", RichLog).clear()
            self.query_one("#algo-status", Static).update("")

    # ── button handler ────────────────────────────────────────────────────

    def on_button_pressed(self, event: Button.Pressed) -> None:
        bid = event.button.id or ""

        if bid == "run-query":
            self._run_query()
        elif bid == "clear-query-output":
            self.query_one("#query-output", RichLog).clear()
            self.query_one("#query-table", DataTable).clear(columns=True)
        elif bid == "create-token":
            self._create_app_token()
        elif bid.startswith("preset-"):
            safe_id = bid.removeprefix("preset-")
            for preset_name in SCOPE_PRESETS:
                if preset_name.lower().replace(" ", "-").replace("/", "-") == safe_id:
                    self._apply_scope_preset(preset_name)
                    break
        elif bid.startswith("algo-"):
            algo_id = bid.removeprefix("algo-")
            self._run_algorithm(algo_id)

    # ── algorithm runner ──────────────────────────────────────────────────

    def _run_algorithm(self, algo_id: str) -> None:
        """الگوریتم انتخاب‌شده را روی گراف انتخاب‌شده اجرا می‌کند."""
        # ── گراف ──
        sel = self.query_one("#graph-select", Select)
        graph_name = str(sel.value) if sel.value and sel.value != Select.BLANK else ""

        # ── پارامترها ──
        raw_params = self.query_one("#graph-param-input", Input).value.strip()
        params: list[str] = []
        if raw_params:
            try:
                parsed = json.loads(raw_params)
                if isinstance(parsed, list):
                    params = [str(x) for x in parsed]
                else:
                    params = [str(parsed)]
            except json.JSONDecodeError:
                # پارامترهای comma-separated هم قبول می‌کنیم
                params = [p.strip() for p in raw_params.split(",") if p.strip()]

        status_msg = (
            f"[yellow]Running [bold]{algo_id}[/bold]"
            f"{' on ' + graph_name if graph_name else ''}…[/yellow]"
        )
        self._write_algo_log(status_msg)
        self.query_one("#algo-status", Static).update(
            f"⏳  {algo_id}" + (f" @ {graph_name}" if graph_name else "")
        )

        self.run_worker(
            lambda: self._algo_worker(algo_id, graph_name, params),
            thread=True,
        )

    def _algo_worker(
        self, algo_id: str, graph_name: str, params: list[str]
    ) -> None:
        try:
            result = self.client.run_graph_algorithm(
                token=self.session.access_token,
                algo_id=algo_id,
                graph_name=graph_name,
                params=params,
            )
        except NexoraAdminClientError as exc:
            self.app.call_from_thread(
                self._write_algo_log, f"[red]Error: {exc}[/red]"
            )
            self.app.call_from_thread(
                self.query_one("#algo-status", Static).update,
                f"❌  {algo_id} failed",
            )
            return

        self.app.call_from_thread(self._render_algo_result, algo_id, result)

    def _render_algo_result(self, algo_id: str, result: dict[str, Any]) -> None:
        log = self.query_one("#algo-output", RichLog)
        elapsed = result.get("elapsedMs", result.get("elapsed_ms", "?"))
        log.write(f"[green]✔ {algo_id} — {elapsed}ms[/green]")
        log.write(json.dumps(result.get("result", result), indent=2, ensure_ascii=False))
        self.query_one("#algo-status", Static).update(f"✔  {algo_id} — {elapsed}ms")

    def _write_algo_log(self, message: str) -> None:
        self.query_one("#algo-output", RichLog).write(message)

    # ── query ─────────────────────────────────────────────────────────────

    def _run_query(self) -> None:
        query = self.query_one("#query-editor", TextArea).text
        self.query_one("#query-output", RichLog).write("[yellow]Running query...[/yellow]")
        self.run_worker(lambda: self._query_worker(query), thread=True)

    def _query_worker(self, query: str) -> None:
        try:
            result = self.client.execute_query(self.session.access_token, query)
        except NexoraAdminClientError as exc:
            self.app.call_from_thread(self._write_query_error, str(exc))
            return
        self.app.call_from_thread(self._render_query_result, result)

    # ── scopes ────────────────────────────────────────────────────────────

    def _load_app_scopes(self) -> None:
        try:
            scopes = self.client.list_app_scopes(self.session.access_token)
        except NexoraAdminClientError as exc:
            self.app.call_from_thread(self._write_token_error, str(exc))
            return
        if scopes:
            self.app.call_from_thread(self._schedule_render_scope_checkboxes, scopes)

    def _schedule_render_scope_checkboxes(self, scopes: list[str]) -> None:
        self.run_worker(self._render_scope_checkboxes(scopes))

    async def _render_scope_checkboxes(self, scopes: list[str]) -> None:
        self.available_scopes = scopes
        self._scope_widgets = {}
        scope_list = self.query_one("#scope-list", Vertical)
        await scope_list.remove_children()
        for scope in scopes:
            checkbox = Checkbox(
                scope,
                id=f"scope-{scope.replace(':', '-')}",
                value=scope in DEFAULT_APP_SCOPES,
            )
            self._scope_widgets[scope] = checkbox
            await scope_list.mount(checkbox)

    def _selected_scopes(self) -> list[str]:
        return [scope for scope, cb in self._scope_widgets.items() if cb.value]

    def _apply_scope_preset(self, preset_name: str) -> None:
        preset = set(SCOPE_PRESETS.get(preset_name, []))
        for scope, cb in self._scope_widgets.items():
            cb.value = scope in preset

    # ── token ─────────────────────────────────────────────────────────────

    def _create_app_token(self) -> None:
        app_id    = self.query_one("#app-id",    Input).value.strip()
        app_name  = self.query_one("#app-name",  Input).value.strip() or None
        expiry_raw= self.query_one("#app-expiry", Input).value.strip()
        scopes    = self._selected_scopes()
        if not scopes:
            self._write_token_error("Select at least one permission scope.")
            return
        try:
            expiry = int(expiry_raw) if expiry_raw else None
        except ValueError:
            self._write_token_error("Expiry must be a number of seconds.")
            return
        self.run_worker(
            lambda: self._token_worker(app_id, app_name, expiry, scopes),
            thread=True,
        )

    def _token_worker(
        self, app_id: str, app_name: str | None, expiry: int | None, scopes: list[str]
    ) -> None:
        try:
            token = self.client.create_app_token(
                self.session.access_token,
                app_id=app_id,
                app_name=app_name,
                expires_in_seconds=expiry,
                scopes=scopes,
            )
        except NexoraAdminClientError as exc:
            self.app.call_from_thread(self._write_token_error, str(exc))
            return
        self.app.call_from_thread(self._write_token, token)

    # ── render helpers ────────────────────────────────────────────────────

    def _render_query_result(self, result: dict[str, Any]) -> None:
        output = self.query_one("#query-output", RichLog)
        table  = self.query_one("#query-table",  DataTable)
        table.clear(columns=True)
        columns = [str(c) for c in result.get("columns", [])]
        rows    = result.get("rows") if isinstance(result.get("rows"), list) else []
        if columns:
            table.add_columns(*columns)
            for row in rows:
                record = row if isinstance(row, dict) else {}
                table.add_row(*(str(record.get(col, "")) for col in columns))
        output.write(
            f"[green]Executed in {result.get('executionTimeMs', 0)}ms · {len(rows)} rows[/green]"
        )
        output.write(json.dumps(result.get("raw"), indent=2, ensure_ascii=False))

    def _write_query_error(self, message: str) -> None:
        self.query_one("#query-output", RichLog).write(f"[red]{message}[/red]")

    def _write_token_error(self, message: str) -> None:
        self.query_one("#token-output", RichLog).write(f"[red]{message}[/red]")

    def _write_token(self, token: dict[str, Any]) -> None:
        self.query_one("#token-output", RichLog).write(json.dumps(token, indent=2))

    def _update_metrics(self, metrics: MonitoringMetrics) -> None:
        self.metrics    = metrics
        self.rps_samples = [*self.rps_samples, metrics.requests_per_second][-60:]
        self.query_one("#database-health", Static).update(
            f"Database\n{'healthy' if metrics.database_healthy else 'unhealthy'}"
        )
        self.query_one("#rps-value",   Static).update(
            f"Traffic\n{metrics.requests_per_second} req/s"
        )
        self.query_one("#active-apps", Static).update(
            f"Active apps\n{len(metrics.active_connections)} in 10s"
        )
        self.query_one("#rps-chart", Sparkline).data = self.rps_samples

        table = self.query_one("#connections-table", DataTable)
        table.clear()
        for conn in metrics.active_connections:
            table.add_row(conn.id, conn.kind, conn.user, conn.address)

    def _set_monitor_status(self, status: str) -> None:
        self.query_one("#database-health", Static).update(f"Monitor\n{status}")


# ══════════════════════════════════════════════════════════════════════════════
# App entry‑point
# ══════════════════════════════════════════════════════════════════════════════

class NexoraCliApp(App[None]):
    TITLE     = "NexoraDB CLI"
    SUB_TITLE = "Admin console, monitor, and query workbench"

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
