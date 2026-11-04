"""Console-wide security, rendering/admission bounds and real Socket.IO lifecycle."""

import asyncio
import importlib
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "NexoraDB/src"))
from textual.app import App
from textual.widgets import (
    Button,
    DataTable,
    Input,
    RichLog,
    Select,
    Static,
    TabbedContent,
)
from nexoradb.cli.api_client import AdminApiClient, AuthSession, NexoraAdminClientError
from nexoradb.cli.console_api import ConsoleApi, graph_query
from nexoradb.cli.monitoring import (
    ConnectionInfo,
    MonitoringClient,
    MonitoringMetrics,
    _TextOnlyPacket,
)
from nexoradb.cli.presentation import json_preview
from nexoradb.cli.query_client import QueryClientError


class FakeApi:
    def __init__(self):
        self.calls, self.closed = [], False

    async def close(self):
        self.closed = True

    async def scopes(self):
        return ["query:execute", "documents:read"]

    async def graphs(self):
        return ["social"]

    async def setup_state(self):
        return {"needsSetup": False}

    async def validate(self, *args):
        return {"valid": True, "cursorSupported": True, "diagnostics": []}

    async def create_token(self, *args):
        self.calls.append(("token", args))
        return {
            "token": "nxapp_private.secret.value",
            "appId": "[red]literal",
            "tokenId": "id",
        }

    async def algorithm(self, *args):
        self.calls.append(("algorithm", args))
        return {"rows": [{"value": "[red]literal"}], "executionTimeMs": 1}


class FakeMonitor:
    def __init__(self, *args, **kwargs):
        self.closed = False

    async def connect(self):
        try:
            await asyncio.Event().wait()
        finally:
            self.closed = True

    async def disconnect(self):
        self.closed = True

    async def refresh(self):
        pass


def console(monkeypatch, api=None):
    module = importlib.import_module("nexoradb.cli.main")
    workspace = importlib.import_module("nexoradb.cli.query_workspace")
    api = api or FakeApi()
    monkeypatch.setattr(module, "ConsoleApi", lambda *args: api)
    monkeypatch.setattr(module, "MonitoringClient", FakeMonitor)
    # Textual dispatches mount handlers across the MRO; this harness starts
    # already authenticated, so suppress the production root's login screen.
    monkeypatch.setattr(module.NexoraCliApp, "on_mount", lambda self: None)
    monkeypatch.setattr(workspace, "QueryClient", lambda *args: FakeApi())

    class Console(module.NexoraCliApp):
        def __init__(self):
            super().__init__("http://localhost")

        def on_mount(self):
            self.panel = module.MainScreen(
                AdminApiClient("http://localhost"),
                AuthSession("session-secret", "[red]root"),
            )
            self.push_screen(self.panel)

    return Console(), api


@pytest.mark.parametrize(
    "name",
    ["social; DROP COLLECTION items; --", "social'", "\x1b]52;c;", "x.y", "x" * 129],
)
def test_graph_identifiers_cannot_inject_statements(name):
    with pytest.raises(ValueError):
        graph_query("GetFriends", name, ["user"])


def test_graph_values_are_bound_and_unknown_algorithms_never_execute(monkeypatch):
    value = "u'; DROP COLLECTION items; --"
    query, bindings = graph_query("GetFriends", "social", [value])
    assert value not in query and bindings == {"p0": value}
    from nexoradb_admin.query_runner import _load_nexoraql_package

    statements = _load_nexoraql_package().parse(query, bindings)
    assert len(statements) == 1
    client = AdminApiClient("http://localhost")
    calls = []
    monkeypatch.setattr(
        client,
        "_request",
        lambda *args, **kwargs: calls.append(kwargs) or {"statementErrors": ["failed"]},
    )
    with pytest.raises(NexoraAdminClientError):
        client.run_graph_algorithm("secret", "GetFriends", "social", [value])
    assert calls[0]["json_body"]["parameters"] == bindings
    with pytest.raises(NexoraAdminClientError):
        client.run_graph_algorithm("secret", "GetFriends; DROP", "social", [])
    assert len(calls) == 1


@pytest.mark.parametrize(
    "field,value",
    [
        ("databaseHealthy", "true"),
        ("requestsPerSecond", True),
        ("requestsPerSecond", -1),
        ("ramUsedBytes", float("nan")),
        ("ssdUsedBytes", float("inf")),
        ("collectionCount", 10**1000),
        ("activeConnections", {}),
        ("activeConnections", [{"id": "x", "user": {}}]),
    ],
)
def test_malformed_monitoring_values_do_not_become_trusted_metrics(field, value):
    payload = {"databaseHealthy": True, field: value}
    with pytest.raises((ValueError, TypeError)):
        MonitoringClient._parse_metrics(payload)


def test_connection_snapshots_are_capped_and_literals_keep_unicode():
    payload = {
        "databaseHealthy": True,
        "activeConnections": [
            {"id": str(i), "user": "[red]نام\u200cخانوادگی\x1b]52;c;"}
            for i in range(2000)
        ],
    }
    metrics = MonitoringClient._parse_metrics(payload)
    assert len(metrics.active_connections) == 200 and metrics.connection_count == 2000
    assert "\x1b" not in metrics.active_connections[0].user
    assert "\u200c" in metrics.active_connections[0].user


@pytest.mark.parametrize(
    "packet", ['51-["metrics",{"_placeholder":true,"num":0}]', "61-[1]"]
)
def test_binary_attachment_accumulation_is_forbidden(packet):
    with pytest.raises(ValueError):
        _TextOnlyPacket(encoded_packet=packet)


def test_json_previews_and_markup_are_bounded():
    text = json_preview(
        {"value": "[red]\x1b]52;c;", "rows": list(range(100000))}, limit=1000
    )
    assert len(text.plain) <= 1100 and "truncated" in text.plain
    assert "[red]" in text.plain and "\x1b" not in text.plain
    assert not text.spans


def test_metrics_bursts_coalesce_hidden_views_pause_and_rows_update_in_place(
    monkeypatch,
):
    async def check():
        app, api = console(monkeypatch)
        async with app.run_test(size=(120, 55)) as pilot:
            screen = app.panel
            rendered = []
            original = screen._update_metrics
            monkeypatch.setattr(
                screen,
                "_update_metrics",
                lambda metrics: rendered.append(metrics) or original(metrics),
            )
            for rate in range(1000):
                screen._queue_metrics(MonitoringMetrics(True, rate))
            assert not rendered
            screen._flush_metrics()
            assert len(rendered) == 1 and rendered[0].requests_per_second == 999
            table = screen.query_one("#connections-table", DataTable)
            monkeypatch.setattr(
                table,
                "clear",
                lambda *args, **kwargs: pytest.fail("must diff rows, not rebuild"),
            )
            original(
                MonitoringMetrics(
                    True, 1, [ConnectionInfo("a", "http", "[red]user\x1b", "127.0.0.1")]
                )
            )
            row = table.rows.copy()
            original(
                MonitoringMetrics(
                    True, 2, [ConnectionInfo("a", "http", "updated", "127.0.0.1")]
                )
            )
            assert table.rows == row and str(table.get_row("a")[2]) == "updated"
            screen.query_one("#main-tabs", TabbedContent).active = "tokens-tab"
            for rate in range(1000):
                screen._queue_metrics(MonitoringMetrics(True, rate))
            count = len(rendered)
            screen._flush_metrics()
            assert len(rendered) == count
            screen.query_one("#main-tabs", TabbedContent).active = "monitor-tab"
            screen._flush_metrics()
            assert len(rendered) == count + 1
            module = importlib.import_module("nexoradb.cli.main")
            tick = [1000]
            screen._last_sample_at = 1000
            screen.rps_samples.clear()
            with monkeypatch.context() as clock:
                clock.setattr(
                    module, "time", SimpleNamespace(monotonic=lambda: tick[0])
                )
                for rate in range(100):
                    tick[0] += 1
                    original(MonitoringMetrics(True, rate))
            assert len(screen.rps_samples) == 60 and screen.rps_samples[0] == 40
            original(MonitoringMetrics(True, source="health"))
            assert "unknown" in str(
                screen.query_one("#database-health", Static).render()
            )
            assert not screen.rps_samples and table.row_count == 0
        assert api.closed and screen.monitor.closed and not screen._tasks

    asyncio.run(check())


def test_tokens_are_hidden_by_default_never_logged_and_clear_on_tab_change(monkeypatch):
    async def check():
        app, _ = console(monkeypatch)
        async with app.run_test(size=(120, 55)) as pilot:
            screen = app.panel
            screen.query_one("#main-tabs", TabbedContent).active = "tokens-tab"
            await pilot.pause()
            screen._write_token(
                {
                    "token": "nxapp_private.secret.value",
                    "appName": "[red]literal\x1b]52;",
                    "tokenId": "id",
                }
            )
            field = screen.query_one("#issued-token", Input)
            assert field.password and field.value == "nxapp_private.secret.value"
            copied = []
            monkeypatch.setattr(app, "copy_to_clipboard", copied.append)
            field.focus()
            await pilot.pause()
            await pilot.press("ctrl+c")
            assert not copied and app.is_running
            log = screen.query_one("#token-output", RichLog)
            printed = "".join(line.text for line in log.lines)
            assert "nxapp_private" not in printed and "\x1b" not in printed
            screen._reveal_token()
            assert not field.password and screen._hide_timer is not None
            field.focus()
            await pilot.pause()
            await pilot.press("x", "backspace")
            assert field.value == "nxapp_private.secret.value"
            # Older Textual has no selection API; modern Input selects on focus.
            if hasattr(field, "selected_text"):
                from textual.widgets._input import Selection

                field.selection = Selection(0, len(field.value))
            field.action_copy()
            assert copied == ["nxapp_private.secret.value"]
            await pilot.press("f1")
            assert field.password
            screen._clear_token()
            assert not field.value and screen._issued_token is None

    asyncio.run(check())


def test_duplicate_token_and_algorithm_actions_have_one_inflight_operation(monkeypatch):
    async def check():
        entered, release = asyncio.Event(), asyncio.Event()

        class Api(FakeApi):
            async def create_token(self, *args):
                self.calls.append(("token", args))
                entered.set()
                await release.wait()
                return {"token": "nxapp_new.token.value"}

        api = Api()
        app, _ = console(monkeypatch, api)
        async with app.run_test(size=(120, 55)) as pilot:
            screen = app.panel
            await pilot.pause()
            screen.query_one("#app-id", Input).value = "billing"
            screen._create_app_token()
            screen._create_app_token()
            await entered.wait()
            assert screen.query_one("#create-token", Button).disabled
            assert len([call for call in api.calls if call[0] == "token"]) == 1
            release.set()
            await screen._tasks["token"]
            assert screen.query_one("#issued-token", Input).password
            screen.query_one("#graph-select", Select).value = "social"
            screen.query_one("#graph-param-input", Input).value = '["user"]'
            screen._run_algorithm("GetFriends")
            screen._run_algorithm("GetFriends")
            await screen._tasks["algorithm"]
            assert len([call for call in api.calls if call[0] == "algorithm"]) == 1

    asyncio.run(check())


def test_login_admission_clears_password_and_cancels_on_exit(monkeypatch):
    async def check():
        module = importlib.import_module("nexoradb.cli.main")
        entered = asyncio.Event()

        class Api(FakeApi):
            async def login(self, username, password):
                self.calls.append(("login", username, password))
                entered.set()
                await asyncio.Event().wait()

        api = Api()
        monkeypatch.setattr(module, "ConsoleApi", lambda *args: api)

        class Login(App):
            def on_mount(self):
                self.panel = module.LoginScreen(AdminApiClient("http://localhost"))
                self.push_screen(self.panel)

        app = Login()
        async with app.run_test(size=(100, 50)) as pilot:
            screen = app.panel
            screen.query_one("#login-username", Input).value = "root"
            screen.query_one("#login-password", Input).value = "private-password"
            screen._login()
            screen._login()
            await entered.wait()
            assert not screen.query_one("#login-password", Input).value
            assert (
                len(api.calls) == 1
                and screen.query_one("#register-button", Button).disabled
            )
        assert api.closed and not screen._tasks

    asyncio.run(check())


class Health:
    def __init__(self):
        self.calls, self.closed = [], False

    async def request(self, method, path, **kwargs):
        self.calls.append(path)
        return (
            {"role": "admin", "status": "active"}
            if path == "/auth/me"
            else {"message": "ok"}
        )

    async def close(self):
        self.closed = True


@pytest.mark.parametrize("prefix", ["", "/database"])
def test_real_async_socketio_metrics_lifecycle_and_refresh_rate(prefix):
    async def check():
        from aiohttp import web
        import socketio

        server = socketio.AsyncServer(async_mode="aiohttp")
        app = web.Application()
        server.attach(
            app,
            socketio_path=prefix.strip("/") + "/socket.io" if prefix else "socket.io",
        )
        calls = []

        @server.event
        async def connect(sid, environ, auth):
            calls.append(("auth", auth))
            await server.emit(
                "metrics", {"databaseHealthy": True, "requestsPerSecond": 7}, to=sid
            )

        @server.on("metrics:refresh")
        async def refresh(sid):
            calls.append(("refresh", sid))

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        received = asyncio.Event()
        samples = []
        health = Health()

        def consume(metrics):
            samples.append(metrics)
            received.set()

        client = MonitoringClient(
            f"http://127.0.0.1:{port}{prefix}",
            "private-session",
            consume,
            lambda status: None,
            http=health,
        )
        task = asyncio.create_task(client.connect())
        try:
            await asyncio.wait_for(received.wait(), 5)
            assert samples[0].requests_per_second == 7 and samples[0].source == "live"
            assert calls[0] == ("auth", {"token": "private-session"})
            await client.refresh()
            await client.refresh()
            await asyncio.sleep(0.05)
            assert len([call for call in calls if call[0] == "refresh"]) == 1
        finally:
            await client.disconnect()
            await asyncio.wait_for(task, 5)
            await runner.cleanup()
        assert health.closed and not client.token

    asyncio.run(check())


def test_websocket_redirect_is_rejected_before_credential_delivery():
    async def check():
        from aiohttp import web

        visited = []

        async def redirect(request):
            raise web.HTTPFound("/other")

        async def other(request):
            visited.append(request)
            return web.Response(text="untrusted")

        app = web.Application()
        app.router.add_get("/socket.io/", redirect)
        app.router.add_get("/other", other)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        client = MonitoringClient(
            f"http://127.0.0.1:{port}",
            "private-session",
            lambda value: None,
            lambda status: None,
        )
        try:
            with pytest.raises(Exception):
                await client._connect_socketio()
            assert not visited
        finally:
            await client.disconnect()
            await runner.cleanup()

    asyncio.run(check())


@pytest.mark.parametrize("denied", [False, True])
def test_websocket_size_limit_and_rejected_auth_have_safe_fallback(denied):
    async def check():
        from aiohttp import web
        import socketio

        server = socketio.AsyncServer(async_mode="aiohttp")
        app = web.Application()
        server.attach(app)

        @server.event
        async def connect(sid, environ, auth):
            if denied:
                raise socketio.exceptions.ConnectionRefusedError(
                    "invalid monitoring token"
                )
            await server.emit(
                "metrics",
                {
                    "databaseHealthy": True,
                    "requestsPerSecond": 17,
                    "padding": "x" * (300 * 1024),
                },
                to=sid,
            )

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        signal = asyncio.Event()
        samples = []
        health = Health()

        def consume(metrics):
            samples.append(metrics)
            signal.set()

        client = MonitoringClient(
            f"http://127.0.0.1:{port}",
            "private",
            consume,
            lambda status: None,
            on_auth_error=signal.set,
            http=health,
        )
        task = asyncio.create_task(client.connect())
        try:
            await asyncio.wait_for(signal.wait(), 8)
            if denied:
                assert client._denied and not health.calls and not samples
            else:
                assert samples and all(sample.source == "health" for sample in samples)
                assert health.calls == ["/auth/me", "/health"]
        finally:
            await client.disconnect()
            await asyncio.wait_for(task, 5)
            await runner.cleanup()

    asyncio.run(check())


def test_fallback_auth_failure_does_not_report_public_health_as_authenticated():
    async def check():
        class Unauthorized(Health):
            async def request(self, method, path, **kwargs):
                self.calls.append(path)
                raise QueryClientError("Expired", status=401)

        expired, samples = [], []
        health = Unauthorized()
        client = MonitoringClient(
            "http://localhost",
            "old",
            samples.append,
            lambda status: None,
            on_auth_error=lambda: expired.append(True),
            http=health,
        )
        await client._poll_once()
        assert (
            expired
            and not samples
            and health.calls == ["/auth/me"]
            and client._stop.is_set()
        )

    asyncio.run(check())


@pytest.mark.parametrize(
    "result",
    [{"scopes": ["query:execute", "[red]injected"]}, {"scopes": ["bad:scope"] * 129}],
)
def test_scope_catalogues_are_bounded_before_widget_creation(result):
    async def check():
        class Transport:
            async def request(self, *args, **kwargs):
                return result

        api = ConsoleApi("http://localhost", client=Transport())
        with pytest.raises(QueryClientError):
            await api.scopes()

    asyncio.run(check())


def test_query_highlighting_coalesces_bursts_without_losing_newest_source(monkeypatch):
    async def check():
        app, _ = console(monkeypatch)
        async with app.run_test(size=(120, 55)) as pilot:
            workspace = app.panel.query_one("#query-workspace")
            editor = workspace.query_one("#query-editor")
            await pilot.pause(0.1)
            highlighted = []
            original = editor.highlight
            monkeypatch.setattr(
                editor,
                "highlight",
                lambda text: highlighted.append(text) or original(text),
            )
            for index in range(20):
                editor.load_text(f"SELECT * FROM items LIMIT {index + 1};")
            await pilot.pause(0.2)
            assert len(highlighted) <= 2 and highlighted[-1] == editor.text

    asyncio.run(check())


def test_clearing_active_preview_preserves_received_record_count(monkeypatch):
    async def check():
        app, _ = console(monkeypatch)
        async with app.run_test(size=(120, 55)) as pilot:
            workspace = app.panel.query_one("#query-workspace")
            workspace.busy = True
            workspace.preview.add([{"x": i} for i in range(100)])
            workspace.clear_output()
            assert workspace.preview.total == 100 and not workspace.preview.rows
            workspace.preview.add([{"x": 100}])
            assert workspace.preview.total == 101
            workspace.busy = False

    asyncio.run(check())
