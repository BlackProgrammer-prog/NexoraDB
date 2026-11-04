"""Textual REPL sharing the web workspace's parser, execution and lease APIs."""

from __future__ import annotations

import asyncio
import json
import time
import uuid

from rich.text import Text
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.message import Message
from textual.widgets import Button, DataTable, RichLog, Select, Static, TextArea

from .query_client import QueryClient, QueryClientError
from .query_editor import (
    MAX_QUERY,
    BoundedTextArea,
    History,
    Preview,
    QueryEditor,
    parameters,
    safe_text,
)


class QueryWorkspace(VerticalScroll):
    class SessionExpired(Message):
        """Ask the host console to discard this admin session."""

    DEFAULT_CSS = """
    QueryWorkspace { height: 1fr; }
    QueryWorkspace #query-editor { height: 8; min-height: 3; }
    QueryWorkspace #query-parameters { height: 3; min-height: 2; }
    QueryWorkspace #query-diagnostics { height: auto; max-height: 3; }
    QueryWorkspace #query-mode { width: 1fr; }
    QueryWorkspace .query-controls {
        height: auto; min-height: 3; layout: grid; grid-size: 3; grid-rows: 3;
    }
    QueryWorkspace .query-controls Button { width: 1fr; min-width: 8; margin: 0; }
    QueryWorkspace #query-navigation { height: 3; }
    QueryWorkspace #query-navigation Button { min-width: 6; width: 1fr; }
    QueryWorkspace #query-status { height: auto; max-height: 3; }
    QueryWorkspace #query-table { height: 1fr; min-height: 3; width: 1fr; }
    QueryWorkspace #query-output { height: 5; min-height: 2; width: 1fr; }
    QueryWorkspace.compact #query-editor { height: 5; }
    QueryWorkspace.compact #query-output { height: 3; }
    """
    BINDINGS = [
        Binding("f5", "run_query", "Run query", priority=True),
        Binding("ctrl+enter", "run_query", "Run query", show=False, priority=True),
        Binding("f6", "stop_query", "Stop/close", priority=True),
        Binding("f7", "next_cursor", "Next server page", priority=True),
        Binding("f8", "history_back", "Previous query", priority=True),
        Binding("f9", "history_forward", "Next query", priority=True),
    ]

    def __init__(self, base_url, token, *, client=None):
        super().__init__(id="query-workspace")
        self.base_url, self.token = base_url, token
        self.client = client
        self.history = History()
        self.preview = Preview()
        self.view_page = 0
        self.cursor = None
        self.key = None
        self.operation = None
        self.diagnostics_task = None
        self.revision = 0
        self.busy = False
        self.stopping = False
        self.closing = False
        self.diagnostic = None
        self.last_render = 0
        self.cleanup_tasks = set()
        self.submitted_query = ""
        self.submitted_parameters = "{}"
        self._highlight_timer = None

    def compose(self):
        yield Static(
            "NexoraQL · Enter: new line · F5: run · F6: stop · F7: next cursor", markup=False
        )
        yield QueryEditor()
        yield Static("Parameters (JSON object; memory only)", markup=False)
        yield BoundedTextArea("{}", id="query-parameters", max_length=65536)
        yield Static("Checking query…", id="query-diagnostics", markup=False)
        yield Select(
            [
                ("Server cursor", "cursor"),
                ("Streaming preview", "stream"),
                ("Buffered", "buffered"),
            ],
            value="cursor",
            allow_blank=False,
            id="query-mode",
        )
        with Vertical(classes="query-controls"):
            yield Button("Run · F5", id="run-query", variant="primary")
            yield Button("Stop/close · F6", id="stop-query", disabled=True)
            yield Button("Next cursor · F7", id="next-query-page", disabled=True)
            yield Button("History ← · F8", id="query-history-back")
            yield Button("History → · F9", id="query-history-forward")
            yield Button("Go to error", id="query-go-error", disabled=True)
            yield Button("Request status", id="query-receipt", disabled=True)
            yield Button("Clear output", id="clear-query-output")
        yield Static(
            "Ready. Cursor results are forward-only; writes finish before delivery.",
            id="query-status",
            markup=False,
        )
        yield Static("Request key: —", id="query-request-key", markup=False)
        yield DataTable(id="query-table", zebra_stripes=True)
        with Horizontal(id="query-navigation"):
            yield Button("← Preview", id="query-preview-back", disabled=True)
            yield Button("Preview →", id="query-preview-forward", disabled=True)
        yield RichLog(id="query-output", max_lines=100, markup=False, highlight=False, wrap=True)

    def on_mount(self):
        if self.client is None:
            self.client = QueryClient(self.base_url, self.token)
        self._schedule_diagnostics()

    def on_resize(self, event):
        self.set_class(event.size.width < 65 or event.size.height < 40, "compact")

    async def on_unmount(self):
        self.closing = True
        tasks = [
            task for task in (self.operation, self.diagnostics_task) if task and not task.done()
        ]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await self._close_cursor()
        await asyncio.gather(*self.cleanup_tasks, return_exceptions=True)
        if self.client:
            await self.client.close()
        self.history = History()
        self.preview = Preview()
        self.submitted_query, self.submitted_parameters = "", "{}"
        self.key = None
        self.token = ""

    def _log(self, text, style=None):
        if not self.closing:
            self.query_one("#query-output", RichLog).write(
                Text(safe_text(str(text), 4000), style=style)
            )

    def _status(self, message):
        if not self.closing:
            self.query_one("#query-status", Static).update(Text(safe_text(message, 1000)))

    def _buttons(self):
        if self.closing:
            return
        self.query_one("#run-query", Button).disabled = self.busy
        self.query_one("#next-query-page", Button).disabled = self.busy or self.cursor is None
        self.query_one("#stop-query", Button).disabled = self.stopping or not (
            self.busy or self.cursor
        )
        self.query_one("#query-receipt", Button).disabled = self.busy or self.key is None
        self.query_one("#query-mode", Select).disabled = self.busy

    def on_text_area_changed(self, event: TextArea.Changed):
        if event.text_area.id not in {"query-editor", "query-parameters"}:
            return
        event.stop()
        if event.text_area.id == "query-editor":
            event.text_area.set_diagnostic(None)
            if self._highlight_timer is None:
                self._highlight_timer = self.set_timer(0.08, self._flush_highlight)
        self._schedule_diagnostics()

    def _flush_highlight(self):
        self._highlight_timer = None
        if not self.closing:
            editor = self.query_one("#query-editor", QueryEditor)
            editor.highlight(editor.text)

    def _schedule_diagnostics(self):
        self.revision += 1
        if self.diagnostics_task:
            self.diagnostics_task.cancel()
        if self.is_mounted and not self.closing:
            revision = self.revision
            self.diagnostics_task = asyncio.create_task(self._diagnose(revision))

    def _input(self):
        query = self.query_one("#query-editor", QueryEditor).text
        params_text = self.query_one("#query-parameters", TextArea).text
        if not query.strip():
            raise ValueError("Query cannot be empty")
        if len(query) > MAX_QUERY:
            raise ValueError("Query exceeds 200,000 characters")
        return query, parameters(params_text), params_text

    async def _diagnose(self, revision):
        try:
            await asyncio.sleep(0.4)
            query, params, _ = self._input()
            result = await self.client.validate(query, params)
            if revision == self.revision and not self.closing:
                self._diagnostics(result)
        except asyncio.CancelledError:
            pass
        except (QueryClientError, ValueError, RecursionError) as exc:
            if revision == self.revision and not self.closing:
                self._diagnostics({"valid": False, "diagnostics": [{"message": str(exc)}]})
                if isinstance(exc, QueryClientError) and exc.status == 401:
                    self.post_message(self.SessionExpired())

    def _diagnostics(self, result):
        diagnostics = result.get("diagnostics", [])
        self.diagnostic = diagnostics[0] if diagnostics else None
        editor = self.query_one("#query-editor", QueryEditor)
        editor.set_diagnostic(self.diagnostic)
        if diagnostics:
            diagnostic = diagnostics[0]
            location = (
                f"Line {diagnostic['line']}, column {diagnostic.get('column') or 1}: "
                if diagnostic.get("line")
                else ""
            )
            message = location + str(diagnostic.get("message", "Invalid query"))
        else:
            message = (
                "Syntax and semantic checks passed; schema and execution are checked when run."
            )
        self.query_one("#query-diagnostics", Static).update(Text(safe_text(message, 1000)))
        self.query_one("#query-go-error", Button).disabled = not bool(
            self.diagnostic and self.diagnostic.get("line")
        )

    def _same_draft(self):
        return (
            not self.closing
            and self.submitted_query == self.query_one("#query-editor", QueryEditor).text
            and self.submitted_parameters == self.query_one("#query-parameters", TextArea).text
        )

    def action_run_query(self):
        if self.busy or self.closing:
            return
        try:
            query, params, params_text = self._input()
        except (ValueError, RecursionError) as exc:
            self._log(str(exc), "red")
            return
        self.busy, self.stopping = True, False
        mode = self.query_one("#query-mode", Select).value
        self.key = str(uuid.uuid4()) if mode != "buffered" else None
        self.query_one("#query-request-key", Static).update(
            Text("Request key: " + (self.key or "none (Buffered has no replay journal)"))
        )
        self.submitted_query, self.submitted_parameters = query, params_text
        self.history.append(query, params_text)
        self.preview, self.view_page = Preview(), 0
        self.render_preview()
        self._status("Running. Stopping delivery does not undo committed writes.")
        if self.key:
            self._log("Request key: " + self.key, "yellow")
        else:
            self._log(
                "Buffered mode has no replay journal. Reconcile outcomes before retrying writes.",
                "yellow",
            )
        self._buttons()
        self.operation = asyncio.create_task(self._run(query, params, mode))

    async def _run(self, query, params, mode):
        try:
            await self._close_cursor()
            validation = await self.client.validate(query, params)
            if self._same_draft():
                self._diagnostics(validation)
            if not validation.get("valid"):
                raise QueryClientError("Query validation failed; nothing was executed")
            if mode != "buffered" and not validation.get("cursorSupported"):
                raise QueryClientError(
                    "Server does not support this program in the selected mode; "
                    "no fallback was executed"
                )
            if mode == "buffered":
                result = await self.client.buffered(query, params)
                self.preview.add(result.get("rows", []))
                self.render_preview()
                self._statement_errors(result)
                self._status(
                    f"Completed in {result.get('executionTimeMs', 0)} ms · "
                    f"{self.preview.total} records"
                )
            elif mode == "cursor":
                result = await self.client.open_cursor(query, params, self.key)
                self._page(result)
            else:
                await asyncio.wait_for(
                    self.client.stream(query, params, self.key, self._batch), timeout=120
                )
                self.render_preview()
                self._status(f"Completed stream · {self.preview.total} records received")
        except asyncio.CancelledError:
            await self._close_cursor()
            self._status(
                "Stopped; displayed results may be incomplete. "
                "Reconcile writes using the request key."
            )
        except Exception as exc:
            await self._failed(exc)
        finally:
            self.busy, self.stopping = False, False
            self._buttons()

    async def _batch(self, rows):
        self.preview.add(rows)
        if time.monotonic() - self.last_render >= 0.1:
            self.render_preview()
            self._status(
                f"Receiving · {self.preview.total} records; "
                "preview limited to 1,000 records / 8 MiB"
            )
            self.last_render = time.monotonic()
        await asyncio.sleep(0)  # Let terminal input/Stop run before reading more data.

    def _statement_errors(self, result):
        errors = result.get("statementErrors", [])
        if errors:
            raise QueryClientError(
                "Statement failed; earlier writes may have committed. "
                + "; ".join(map(str, errors))
            )

    def _page(self, result):
        # Remember an advanced lease before handling logical errors or rendering.
        self.cursor = result if result.get("cursorId") else None
        rows = result.get("rows", [])
        if rows or not self.preview.total:
            self.preview, self.view_page = Preview(), 0
            self.preview.add(rows)
        self.render_preview()
        self._statement_errors(result)
        self._status(
            f"Server page {result.get('sequence')} · {result.get('totalRows')} records received · "
            + ("idle expiry 30s; lifetime 120s" if self.cursor else "complete")
        )

    def action_next_cursor(self):
        if self.busy or not self.cursor or self.closing:
            return
        self.busy, self.stopping = True, False
        self._buttons()
        self.operation = asyncio.create_task(self._next())

    async def _next(self):
        try:
            result = await self.client.next_page(
                self.cursor["cursorId"], self.cursor["sequence"] + 1
            )
            self._page(result)
        except asyncio.CancelledError:
            await self._close_cursor()
            self._status("Stopped. Cursor closed; no page was retried.")
        except Exception as exc:
            await self._failed(exc)
        finally:
            self.busy, self.stopping = False, False
            self._buttons()

    async def _failed(self, exc):
        details = exc.details if isinstance(exc, QueryClientError) else None
        if not self.cursor and isinstance(details, dict) and details.get("cursorId"):
            self.cursor = {"cursorId": details["cursorId"]}
        await self._close_cursor()
        if self.preview.total:
            self.render_preview()
        self._log(str(exc), "red")
        self._status(
            "Failed or incomplete. No automatic retry. Reconcile writes before running again."
        )
        if isinstance(exc, QueryClientError) and exc.status == 401 and not self.closing:
            self.post_message(self.SessionExpired())
        if isinstance(details, dict) and isinstance(details.get("queryError"), dict):
            if self._same_draft():
                self._diagnostics({"diagnostics": [details["queryError"]]})

    async def _close_cursor(self):
        cursor, self.cursor = self.cursor, None
        if cursor:

            async def close():
                try:
                    await self.client.close_cursor(cursor["cursorId"])
                except Exception:
                    self._log(
                        "Cursor close was not acknowledged; server expiry is the fallback.",
                        "yellow",
                    )

            task = asyncio.create_task(close())
            self.cleanup_tasks.add(task)
            task.add_done_callback(self.cleanup_tasks.discard)
            await asyncio.shield(task)

    def action_stop_query(self):
        if self.stopping or self.closing:
            return
        if self.busy and self.operation and not self.operation.done():
            self.stopping = True
            self._status("Cancelling transport; committed writes remain committed.")
            self._buttons()
            self.operation.cancel()
        elif self.cursor:
            self.busy = True
            self.operation = asyncio.create_task(self._stop_idle())
            self._buttons()

    async def _stop_idle(self):
        try:
            await self._close_cursor()
            self._status("Cursor closed.")
        finally:
            self.busy = False
            self._buttons()

    def render_preview(self):
        if self.closing:
            return
        rows = self.preview.snapshot()
        self.view_page = max(0, min(self.view_page, max(0, (len(rows) - 1) // 50)))
        page = rows[self.view_page * 50 : (self.view_page + 1) * 50]
        columns = []
        for row in page:
            for key in row:
                if key not in columns:
                    columns.append(key)
                if len(columns) >= 50:
                    break
            if len(columns) >= 50:
                break
        table = self.query_one("#query-table", DataTable)
        table.clear(columns=True)
        if columns:
            table.add_columns(*(Text(safe_text(column, 100)) for column in columns))
            for row in page:
                table.add_row(*(Text(safe_text(row.get(column, ""), 1000)) for column in columns))
        self.query_one("#query-preview-back", Button).disabled = self.view_page == 0
        self.query_one("#query-preview-forward", Button).disabled = (
            self.view_page + 1
        ) * 50 >= len(rows)
        table.border_title = (
            f"{len(rows)} retained / {self.preview.total} received · preview {self.view_page + 1}"
        )

    def clear_output(self):
        received = self.preview.total if self.busy else 0
        self.preview, self.view_page = Preview(), 0
        self.preview.total = received
        self.render_preview()
        self.query_one("#query-output", RichLog).clear()
        if self.key:
            self._log("Request key: " + self.key)

    def _history(self, delta):
        if self.busy:
            return
        editor = self.query_one("#query-editor", QueryEditor)
        params = self.query_one("#query-parameters", TextArea)
        query, data = self.history.move(delta, (editor.text, params.text))
        editor.load_text(query)
        params.load_text(data)

    def action_history_back(self):
        self._history(-1)

    def action_history_forward(self):
        self._history(1)

    async def _receipt(self):
        try:
            self._log(json.dumps(await self.client.receipt(self.key), ensure_ascii=False), "cyan")
        except Exception as exc:
            self._log(str(exc), "red")
            if isinstance(exc, QueryClientError) and exc.status == 401 and not self.closing:
                self.post_message(self.SessionExpired())
        finally:
            self.busy = False
            self._buttons()

    def on_button_pressed(self, event: Button.Pressed):
        event.stop()
        action = event.button.id
        if action == "run-query":
            self.action_run_query()
        elif action == "stop-query":
            self.action_stop_query()
        elif action == "next-query-page":
            self.action_next_cursor()
        elif action == "clear-query-output":
            self.clear_output()
        elif action == "query-history-back":
            self.action_history_back()
        elif action == "query-history-forward":
            self.action_history_forward()
        elif action == "query-preview-back":
            self.view_page -= 1
            self.render_preview()
        elif action == "query-preview-forward":
            self.view_page += 1
            self.render_preview()
        elif action == "query-go-error" and self.diagnostic:
            editor = self.query_one("#query-editor", QueryEditor)
            line = max(0, min(editor.document.line_count - 1, self.diagnostic["line"] - 1))
            column = max(
                0,
                min(len(editor.document.get_line(line)), (self.diagnostic.get("column") or 1) - 1),
            )
            editor.move_cursor((line, column))
            editor.focus()
        elif action == "query-receipt" and self.key and not self.busy:
            self.busy = True
            self._buttons()
            self.operation = asyncio.create_task(self._receipt())
