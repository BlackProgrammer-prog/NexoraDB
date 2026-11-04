"""Headless terminal behavior and bounded wire protocol (no terminal emulator needed)."""

import asyncio
import json
import os
from pathlib import Path
import sys

import httpx
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "NexoraDB/src"))
from textual.app import App
from textual.widgets import DataTable, Select, TextArea, Button, Static
from nexoradb.cli.query_client import QueryClient, QueryClientError
from nexoradb.cli.query_editor import (
    History,
    Preview,
    QueryEditor,
    parameters,
    safe_text,
)
from nexoradb.cli.query_workspace import QueryWorkspace


class FakeClient:
    def __init__(self):
        self.calls, self.closed = [], False

    async def validate(self, query, params):
        return {"valid": True, "cursorSupported": True, "diagnostics": []}

    async def open_cursor(self, query, params, key):
        self.calls.append(("open", query, params, key))
        return {
            "cursorId": "lease",
            "sequence": 1,
            "rows": [{"value": i} for i in range(100)],
            "totalRows": 100,
            "hasMore": True,
        }

    async def next_page(self, identifier, sequence):
        self.calls.append(("next", identifier, sequence))
        return {
            "cursorId": None,
            "sequence": sequence,
            "rows": [{"value": 100}],
            "totalRows": 101,
            "hasMore": False,
        }

    async def close_cursor(self, identifier):
        self.calls.append(("close", identifier))

    async def close(self):
        self.closed = True


class TerminalApp(App):
    def __init__(self, client):
        super().__init__()
        self.workspace = QueryWorkspace("http://localhost", "secret", client=client)

    def compose(self):
        yield self.workspace


def test_terminal_multiline_keys_cursor_navigation_and_cleanup():
    async def check():
        client = FakeClient()
        app = TerminalApp(client)
        async with app.run_test(size=(100, 50)) as pilot:
            editor = app.query_one(QueryEditor)
            editor.focus()
            editor.load_text("SELECT * FROM items;")
            await pilot.press("end", "enter")
            assert "\n" in editor.text and not client.calls
            await pilot.press("escape")
            assert app.focused is app.query_one("#query-parameters", TextArea)
            editor.focus()
            await pilot.press("f5")
            await pilot.pause()
            assert len([c for c in client.calls if c[0] == "open"]) == 1
            assert app.query_one("#query-table", DataTable).row_count == 50
            assert not app.query_one("#next-query-page", Button).disabled
            app.workspace.view_page = 1
            app.workspace.render_preview()
            assert (
                str(app.query_one("#query-table", DataTable).get_cell_at((0, 0)))
                == "50"
            )
            await pilot.press("f7")
            await pilot.pause()
            assert ("next", "lease", 2) in client.calls
            assert app.workspace.cursor is None
            app.workspace.action_run_query()
            await pilot.pause()
        assert ("close", "lease") in client.calls and client.closed

    asyncio.run(check())


def test_preview_and_history_have_global_budgets():
    preview = Preview(max_rows=3, max_bytes=80)
    preview.add([{"x": i} for i in range(20)])
    assert preview.snapshot() == [{"x": 17}, {"x": 18}, {"x": 19}]
    assert preview.total == 20 and preview.bytes <= 80
    preview.add([{"x": "x" * 100}])
    assert not preview.rows and preview.bytes == 0
    history = History()
    for index in range(100):
        history.append(str(index), "{}")
    assert len(history.entries) == 50
    assert history.move(-1, ("draft", "{}")) == ("99", "{}")
    assert history.move(1, ("99", "{}")) == ("draft", "{}")


@pytest.mark.parametrize(
    "text",
    [
        "[]",
        "null",
        '{"value":NaN}',
        '{"value":Infinity}',
        '{"value":1e999}',
        "x" * 65537,
    ],
)
def test_parameters_reject_invalid_or_unbounded_values(text):
    with pytest.raises(ValueError):
        parameters(text)


def test_terminal_data_is_literal_and_controls_are_suppressed():
    assert safe_text("[red]literal[/red]") == "[red]literal[/red]"
    assert "\x1b" not in safe_text("\x1b]52;c;secret\x07")
    assert "\u202e" not in safe_text("name\u202eabc")
    assert safe_text({"unicode": "سلام"}) == '{"unicode":"سلام"}'
    assert safe_text('نام‌خانوادگی 🧑‍💻') == 'نام‌خانوادگی 🧑‍💻'
    assert safe_text('abcdef', limit=4) == 'abc…'


class Chunks(httpx.AsyncByteStream):
    def __init__(self, chunks):
        self.chunks, self.closed = chunks, False

    async def __aiter__(self):
        for chunk in self.chunks:
            yield chunk

    async def aclose(self):
        self.closed = True


def test_transport_split_unicode_frames_and_request_headers():
    async def check():
        encoded = ('{"document":{"name":"سلام 😀"}}\n{"end":true}\n').encode()
        body = Chunks([encoded[i : i + 1] for i in range(len(encoded))])
        calls = []

        def handle(request):
            calls.append(request)
            return httpx.Response(
                200, headers={"content-type": "application/x-ndjson"}, stream=body
            )

        client = QueryClient(
            "http://localhost", "secret", transport=httpx.MockTransport(handle)
        )
        rows = []

        async def consume(batch):
            rows.extend(batch)

        try:
            await client.stream(
                "SELECT * FROM items;", {"x": 17}, "explicit-key", consume
            )
            assert rows == [{"name": "سلام 😀"}]
            assert calls[0].headers["Idempotency-Key"] == "explicit-key"
            assert calls[0].headers["Authorization"] == "Bearer secret"
            assert json.loads(calls[0].content)["parameters"] == {"x": 17}
            assert body.closed and len(calls) == 1
        finally:
            await client.close()

    asyncio.run(check())


@pytest.mark.parametrize(
    "body",
    [
        b'{"document":{}}\n',
        b'{"end":true,"document":{}}\n',
        b'{"end":true}\n{"document":{}}\n',
        b'{"document":[]}\n',
        b'{"document":{"x":NaN}}\n',
        b"x" * (2 * 1024 * 1024 + 1),
        b'{"document":{"x":"\xff"}}\n',
    ],
)
def test_stream_never_reports_invalid_or_truncated_output_as_success(body):
    async def check():
        stream = Chunks([body])
        client = QueryClient(
            "http://localhost",
            "secret",
            transport=httpx.MockTransport(
                lambda request: httpx.Response(
                    200, headers={"content-type": "application/x-ndjson"}, stream=stream
                )
            ),
        )

        async def consume(batch):
            pass

        try:
            with pytest.raises(QueryClientError):
                await client.stream("SELECT * FROM items;", {}, "key", consume)
            assert stream.closed
        finally:
            await client.close()

    asyncio.run(check())


def test_transport_redirects_cookies_errors_and_budgets_are_bounded():
    async def check():
        calls = []

        def handle(request):
            calls.append(request)
            if request.url.path == "/redirect":
                return httpx.Response(
                    302, headers={"location": "https://other.example/stolen"}
                )
            if request.url.path == "/large":
                return httpx.Response(
                    200,
                    headers={"content-type": "application/json"},
                    content=b"x" * (8 * 1024 * 1024 + 1),
                )
            return httpx.Response(
                200, json={}, headers={"Set-Cookie": "session=server-secret"}
            )

        client = QueryClient(
            "http://localhost", "secret", transport=httpx.MockTransport(handle)
        )
        try:
            await client.request("GET", "/cookie")
            await client.request("GET", "/cookie")
            assert all("cookie" not in call.headers for call in calls)
            with pytest.raises(QueryClientError) as redirect:
                await client.request("GET", "/redirect")
            assert redirect.value.status == 302 and len(calls) == 3
            with pytest.raises(QueryClientError):
                await client.request("GET", "/large")
            with pytest.raises(QueryClientError):
                await client.request("GET", "https://other.example")
            with pytest.raises(QueryClientError):
                await client.request("POST", "/large", {"value": "x" * 1024 * 1024})
            assert len(calls) == 4
        finally:
            await client.close()

    asyncio.run(check())


def test_transport_cancellation_closes_stalled_stream_without_retry():
    async def check():
        entered, closed = asyncio.Event(), asyncio.Event()

        class Stalled(httpx.AsyncByteStream):
            async def __aiter__(self):
                entered.set()
                await asyncio.Event().wait()
                yield b""

            async def aclose(self):
                closed.set()

        client = QueryClient(
            "http://localhost",
            "secret",
            transport=httpx.MockTransport(
                lambda request: httpx.Response(
                    200,
                    headers={"content-type": "application/x-ndjson"},
                    stream=Stalled(),
                )
            ),
        )

        async def consume(batch):
            pass

        try:
            task = asyncio.create_task(
                client.stream("SELECT * FROM items;", {}, "key", consume)
            )
            await entered.wait()
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            assert closed.is_set()
        finally:
            await client.close()

    asyncio.run(check())


@pytest.mark.parametrize(
    "change",
    [
        {"sequence": 9},
        {"hasMore": False},
        {"rows": ["bad"]},
        {"totalRows": -1},
        {"cursorId": "../auth"},
        {"statementErrors": "bad"},
    ],
)
def test_cursor_protocol_errors_do_not_silently_advance_or_retry(change):
    async def check():
        page = {
            "cursorId": "lease",
            "hasMore": True,
            "sequence": 1,
            "rows": [{}],
            "totalRows": 1,
            **change,
        }
        client = QueryClient(
            "http://localhost",
            "secret",
            transport=httpx.MockTransport(
                lambda request: httpx.Response(200, json=page)
            ),
        )
        try:
            with pytest.raises(QueryClientError):
                await client.open_cursor("SELECT * FROM items;", {}, "key")
        finally:
            await client.close()

    asyncio.run(check())


def test_highlighting_tracks_multiline_comments_strings_and_unicode_diagnostics():
    editor = QueryEditor()
    editor.load_text("/* SELECT\nFROM */\nSELECT 'SELECT 😀' FROM users;")
    editor.highlight(editor.text)
    assert any(span.style == "italic #94a3b8" for span in editor.get_line(1).spans)
    assert any(
        span.style == "#93c5fd" and span.start == 0 for span in editor.get_line(2).spans
    )
    assert any(span.style == "#86efac" for span in editor.get_line(2).spans)
    editor.set_diagnostic({"line": 3, "column": 17})
    assert any(
        span.style == "underline red" and span.start == 16
        for span in editor.get_line(2).spans
    )
    with pytest.raises(ValueError):
        editor.load_text("x" * 200001)
    with pytest.raises(ValueError):
        editor.load_text("SELECT \x1b]52;")


def test_stopping_stream_preserves_partial_preview_and_key():
    async def check():
        entered, closed = asyncio.Event(), asyncio.Event()

        class Client(FakeClient):
            async def stream(self, query, params, key, consume):
                try:
                    await consume([{"row": {"value": "[red]literal\x1b]52;"}}])
                    entered.set()
                    await asyncio.Event().wait()
                finally:
                    closed.set()

        client = Client()
        app = TerminalApp(client)
        async with app.run_test(size=(80, 45)) as pilot:
            app.query_one("#query-mode", Select).value = "stream"
            app.workspace.action_run_query()
            app.workspace.action_run_query()
            await entered.wait()
            key = app.workspace.key
            app.workspace.action_stop_query()
            await pilot.pause()
            assert closed.is_set() and not app.workspace.busy
            assert app.workspace.key == key and app.workspace.preview.total == 1
            cell = str(app.query_one("#query-table", DataTable).get_cell_at((0, 0)))
            assert "[red]literal" in cell and "\x1b" not in cell
            assert key in str(app.query_one("#query-request-key", Static).render())

    asyncio.run(check())


def test_stream_retention_rendering_and_local_pagination_are_bounded():
    async def check():
        class Client(FakeClient):
            async def stream(self, query, params, key, consume):
                for start in range(0, 3000, 128):
                    await consume(
                        [{"value": i} for i in range(start, min(start + 128, 3000))]
                    )

        app = TerminalApp(Client())
        async with app.run_test(size=(80, 50)) as pilot:
            app.query_one("#query-mode", Select).value = "stream"
            app.workspace.action_run_query()
            await app.workspace.operation
            assert app.workspace.preview.total == 3000
            assert len(app.workspace.preview.rows) == 1000
            assert app.query_one("#query-table", DataTable).row_count == 50
            assert (
                str(app.query_one("#query-table", DataTable).get_cell_at((0, 0)))
                == "2000"
            )
            assert not app.query_one("#query-preview-forward", Button).disabled

    asyncio.run(check())


def test_concurrent_next_clicks_send_one_request_and_failure_closes_lease():
    async def check():
        entered, release = asyncio.Event(), asyncio.Event()

        class Client(FakeClient):
            async def next_page(self, identifier, sequence):
                self.calls.append(("next", identifier, sequence))
                entered.set()
                await release.wait()
                raise QueryClientError("Lost page response")

        client = Client()
        app = TerminalApp(client)
        async with app.run_test(size=(80, 45)) as pilot:
            app.workspace.action_run_query()
            await app.workspace.operation
            app.workspace.action_next_cursor()
            app.workspace.action_next_cursor()
            await entered.wait()
            release.set()
            await app.workspace.operation
            assert [c for c in client.calls if c[0] == "next"] == [("next", "lease", 2)]
            assert ("close", "lease") in client.calls and app.workspace.cursor is None

    asyncio.run(check())


def test_late_cursor_response_is_closed_during_unmount():
    async def check():
        entered = asyncio.Event()

        class Client(FakeClient):
            async def open_cursor(self, query, params, key):
                entered.set()
                try:
                    await asyncio.Event().wait()
                except asyncio.CancelledError:
                    return await super().open_cursor(query, params, key)

        client = Client()
        app = TerminalApp(client)
        async with app.run_test(size=(80, 45)) as pilot:
            app.workspace.action_run_query()
            await entered.wait()
        assert ("close", "lease") in client.calls and client.closed

    asyncio.run(check())


@pytest.mark.parametrize("size", [(40, 20), (64, 28), (120, 45)])
def test_small_terminals_keep_controls_accessible_by_scrolling(size):
    async def check():
        app = TerminalApp(FakeClient())
        async with app.run_test(size=size) as pilot:
            button = app.query_one("#query-receipt", Button)
            button.scroll_visible(animate=False)
            await pilot.pause()
            assert button.region.width > 0
            assert button.region.y >= 0 and button.region.bottom <= size[1]

    asyncio.run(check())


def test_main_screen_routes_function_keys_without_duplicate_execution(monkeypatch):
    from nexoradb.cli.main import MainScreen
    from nexoradb.cli.api_client import AdminApiClient, AuthSession
    import nexoradb.cli.query_workspace as module

    client = FakeClient()
    monkeypatch.setattr(module, "QueryClient", lambda *args, **kwargs: client)
    monkeypatch.setattr(
        MainScreen,
        "on_mount",
        lambda self: self._apply_responsive_classes(self.size.width),
    )

    class Console(App):
        def on_mount(self):
            self.push_screen(
                MainScreen(
                    AdminApiClient("http://localhost"), AuthSession("secret", "root")
                )
            )

    async def check():
        app = Console()
        async with app.run_test(size=(120, 50)) as pilot:
            await pilot.press("f2")
            app.set_focus(None)
            await pilot.press("f5")
            await pilot.pause()
            assert len([call for call in client.calls if call[0] == "open"]) == 1
            await pilot.press("f7")
            await pilot.pause()
            assert ("next", "lease", 2) in client.calls
        assert client.closed

    asyncio.run(check())


def test_diagnostics_ignore_obsolete_unicode_error_and_jump_to_current_error():
    async def check():
        entered, release = asyncio.Event(), asyncio.Event()

        class Client(FakeClient):
            async def validate(self, query, params):
                if "obsolete" in query:
                    entered.set()
                    try:
                        await release.wait()
                    except asyncio.CancelledError:
                        await release.wait()
                    return {
                        "valid": False,
                        "diagnostics": [
                            {"message": "obsolete error", "line": 1, "column": 1}
                        ],
                    }
                if "FRUM" in query:
                    return {
                        "valid": False,
                        "diagnostics": [
                            {"message": "Expected FROM", "line": 2, "column": 1}
                        ],
                    }
                return await super().validate(query, params)

        app = TerminalApp(Client())
        async with app.run_test(size=(80, 45)) as pilot:
            editor = app.query_one(QueryEditor)
            editor.load_text("SELECT obsolete FROM items;")
            await entered.wait()
            editor.load_text("SELECT '😀'\nFRUM items;")
            await pilot.pause(0.5)
            release.set()
            await pilot.pause()
            assert app.workspace.diagnostic["message"] == "Expected FROM"
            await pilot.click("#query-go-error")
            assert editor.cursor_location == (1, 0)

    asyncio.run(check())


def test_statement_failure_keeps_key_and_prior_results_without_retry():
    async def check():
        class Client(FakeClient):
            async def open_cursor(self, query, params, key):
                result = await super().open_cursor(query, params, key)
                result["statementErrors"] = ["duplicate key"]
                return result

        client = Client()
        app = TerminalApp(client)
        async with app.run_test(size=(80, 45)) as pilot:
            app.workspace.action_run_query()
            await app.workspace.operation
            assert app.workspace.preview.total == 100 and app.workspace.key
            assert ("close", "lease") in client.calls
            assert len([call for call in client.calls if call[0] == "open"]) == 1
            assert app.workspace.cursor is None

    asyncio.run(check())


def test_real_admin_parser_native_join_cursor_stream_and_write_replay(tmp_path):
    from fastapi.testclient import TestClient
    from nexoradb_admin.app import create_app
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import load_native_module
    from nexoradb_admin.security import create_access_token

    root = Path(__file__).resolve().parents[1]
    binary = Path(
        os.environ.get(
            "NEXORADB_TEST_NATIVE_PATH",
            root
            / "NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so",
        )
    )
    if not binary.exists():
        pytest.skip("Native Release extension is not built")
    settings = AdminApiSettings(
        auth_secret="x" * 48,
        db_path=tmp_path / "db",
        graph_dir=tmp_path / "graphs",
        native_module_path=binary,
    )
    native = load_native_module(settings)
    engine = native.DocEngine(str(settings.db_path))
    app = create_app(settings=settings, engine=engine)
    with TestClient(app) as setup:
        assert (
            setup.post(
                "/auth/register",
                json={
                    "firstName": "Test",
                    "lastName": "Admin",
                    "email": "terminal@example.test",
                    "password": "StrongPass123",
                },
            ).status_code
            == 201
        )
        token = create_access_token(username="root", role="admin", settings=settings)
        assert engine.create_collection("items").success
        assert engine.create_collection("targets").success
        assert engine.insert_one("items", '{"_id":"a","target":"b"}').success
        assert engine.insert_one("targets", '{"_id":"b","value":17}').success

        async def check():
            client = QueryClient(
                "http://test", token, transport=httpx.ASGITransport(app=app)
            )
            try:
                invalid = await client.validate("SELECT * FRUM items;", {})
                assert not invalid["valid"] and invalid["diagnostics"][0]["column"]
                query = "SELECT * FROM items LOOKUP JOIN targets ON target = _id;"
                first = await client.open_cursor(query, {}, "read-key")
                assert first["rows"][0]["__joined__"]["value"] == 17
                while first["cursorId"]:
                    first = await client.next_page(
                        first["cursorId"], first["sequence"] + 1
                    )
                rows = []

                async def consume(batch):
                    rows.extend(batch)

                await client.stream(query, {}, "stream-key", consume)
                assert rows[0]["__joined__"]["value"] == 17
                write = "INSERT INTO items VALUES ($record); SELECT * FROM items WHERE _id = 'once';"
                payload = {"record": {"_id": "once", "value": 9}}
                first = await client.open_cursor(write, payload, "terminal-write-key")
                replay = await client.open_cursor(write, payload, "terminal-write-key")
                assert first["rows"] == replay["rows"]
                status = await client.receipt("terminal-write-key")
                assert status["status"] == "completed"
                assert engine.get_collection_size("items") == 2
                for page in (first, replay):
                    if page["cursorId"]:
                        await client.close_cursor(page["cursorId"])
            finally:
                await client.close()

        asyncio.run(check())
    assert not app.state.query_cursors.entries


def test_paste_limit_rejects_entire_edit_without_changing_query_semantics():
    async def check():
        app = TerminalApp(FakeClient())
        async with app.run_test(size=(80, 45)) as pilot:
            editor = app.query_one(QueryEditor)
            original = editor.text
            editor.insert("x" * 200001)
            assert editor.text == original
            editor.insert("\x1b]52;c;secret\x07")
            assert editor.text == original
            editor.insert("SELECT ")
            assert editor.text != original

    asyncio.run(check())


def test_sync_admin_auth_transport_does_not_forward_credentials_on_redirect(
    monkeypatch,
):
    from nexoradb.cli import api_client

    calls = []
    original = httpx.Client

    def handle(request):
        calls.append(request)
        return httpx.Response(302, headers={"Location": "https://other.example/stolen"})

    monkeypatch.setattr(
        api_client.httpx,
        "Client",
        lambda **kwargs: original(transport=httpx.MockTransport(handle), **kwargs),
    )
    client = api_client.AdminApiClient("http://localhost")
    with pytest.raises(api_client.NexoraAdminClientError):
        client._request("GET", "/apps/scopes", token="private-token")
    assert len(calls) == 1 and calls[0].url.host == "localhost"
    assert calls[0].headers["Authorization"] == "Bearer private-token"


def test_stream_batches_obey_byte_budget_and_keep_backpressure():
    async def check():
        first = (
            json.dumps({"document": {"payload": "x" * (1100 * 1024)}}).encode() + b"\n"
        )
        stream = Chunks([first + first + b'{"end":true}\n'])
        client = QueryClient(
            "http://localhost",
            "secret",
            transport=httpx.MockTransport(
                lambda request: httpx.Response(
                    200, headers={"content-type": "application/x-ndjson"}, stream=stream
                )
            ),
        )
        batches = []

        async def consume(batch):
            await asyncio.sleep(0)
            batches.append(len(batch))

        try:
            await client.stream("SELECT * FROM items;", {}, "key", consume)
            assert batches == [1, 1]
        finally:
            await client.close()

    asyncio.run(check())
