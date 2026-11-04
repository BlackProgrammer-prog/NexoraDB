"""Terminal-safe NexoraQL highlighting, diagnostics and bounded session state."""

import json
import re
import unicodedata
from collections import deque

from textual.widgets import TextArea

from .query_client import strict_json

MAX_QUERY = 200_000
KEYWORDS = frozenset(
    (
        "SELECT FROM WHERE LIMIT SKIP INSERT INTO VALUES UPDATE SET DELETE "
        "CREATE DROP ALTER COLLECTION INDEX UNIQUE ON SHOW COLLECTIONS EXISTS COUNT "
        "DESCRIBE USE GRAPH GRAPHS LIVE STATIC DIRECTED UNDIRECTED HETEROGENEOUS MAP "
        "NODE EDGE KEY PROPERTIES SOURCE TARGET AS BUILD REFRESH RENDER TRAVERSE OUT "
        "IN BOTH DEPTH RUN LOCK JOB WITH BEGIN COMMIT ROLLBACK TRANSACTION AND OR NOT "
        "IS NULL TRUE FALSE LOOKUP JOIN SYSTEM STATUS INFO FOREIGN REFERENCES RESTRICT"
    ).split()
)
TOKENS = re.compile(
    r"--[^\n]*|/\*[\s\S]*?(?:\*/|\Z)|'(?:\\[\s\S]|[^'\\])*(?:'|\Z)|"
    r'"(?:\\[\s\S]|[^"\\])*(?:"|\Z)|\$[A-Za-z_]\w*|'
    r"-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|[A-Za-z_]\w*"
)


def safe_text(value, limit=2000):
    """Render data as literal text; suppress terminal controls, OSC and bidi overrides."""
    text = (
        value
        if isinstance(value, str)
        else json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    )
    clipped = len(text) > limit
    visible = text[: max(0, limit - 1)] if clipped else text
    return "".join(
        "\ufffd"
        if unicodedata.category(char) in {"Cc", "Cs"}
        or (unicodedata.category(char) == "Cf" and char not in "\u200c\u200d")
        else char
        for char in visible
    ) + ("…" if clipped and limit else "")


def parameters(text):
    if len(text.encode()) > 65536:
        raise ValueError("Parameters exceed 64 KiB")
    value = strict_json(text or "{}")
    if not isinstance(value, dict):
        raise ValueError("Parameters must be a JSON object")
    return value


class Preview:
    def __init__(self, max_rows=1000, max_bytes=8 * 1024 * 1024):
        self.rows = deque()
        self.total = self.bytes = 0
        self.max_rows, self.max_bytes = max_rows, max_bytes

    def add(self, batch):
        for row in batch:
            self.total += 1
            size = len(json.dumps(row, ensure_ascii=False, allow_nan=False).encode())
            if size > self.max_bytes:
                self.rows.clear()
                self.bytes = 0
                continue
            while self.rows and (
                len(self.rows) >= self.max_rows or self.bytes + size > self.max_bytes
            ):
                _, old = self.rows.popleft()
                self.bytes -= old
            self.rows.append((row, size))
            self.bytes += size

    def snapshot(self):
        return [row for row, _ in self.rows]


class History:
    def __init__(self):
        self.entries = []
        self.position = 0
        self.draft = ("", "{}")

    def append(self, query, params):
        entry = (query, params)
        if not self.entries or self.entries[-1] != entry:
            self.entries.append(entry)
        while (
            len(self.entries) > 50
            or sum(len(q.encode()) + len(p.encode()) for q, p in self.entries) > 1024 * 1024
        ):
            self.entries.pop(0)
        self.position = len(self.entries)

    def move(self, delta, draft):
        if self.position == len(self.entries):
            self.draft = draft
        self.position = max(0, min(len(self.entries), self.position + delta))
        return self.entries[self.position] if self.position < len(self.entries) else self.draft


class BoundedTextArea(TextArea):
    def __init__(self, text="", *, max_length=MAX_QUERY, **kwargs):
        self.max_length = max_length
        super().__init__(text, **kwargs)

    def _check_text(self, text):
        if len(text) > self.max_length:
            raise ValueError(f"Editor limit is {self.max_length} characters")
        if any(
            (unicodedata.category(char) == "Cc" and char not in "\n\t\r")
            or char in "\u202a\u202b\u202c\u202d\u202e\u2066\u2067\u2068\u2069"
            for char in text
        ):
            raise ValueError(
                "Terminal controls and bidi overrides are not accepted; use escaped JSON parameters"
            )

    def load_text(self, text):
        self._check_text(text)
        return super().load_text(text)

    def edit(self, edit):
        try:
            self._check_text(edit.text)
            removed = self.document.get_text_range(edit.top, edit.bottom)
            if len(self.text) - len(removed) + len(edit.text) > self.max_length:
                raise ValueError(f"Editor limit is {self.max_length} characters")
        except ValueError as exc:
            self.notify(str(exc), severity="warning")
            return super().replace("", edit.top, edit.top)
        return super().edit(edit)


class QueryEditor(BoundedTextArea):
    """Public get_line hook avoids an optional tree-sitter SQL grammar dependency."""

    def __init__(self):
        self.spans = {}
        self.diagnostic = None
        super().__init__(
            "SHOW COLLECTIONS;", id="query-editor", show_line_numbers=True, tab_behavior="indent"
        )
        self.highlight(self.text)

    def highlight(self, source):
        self.spans = {}
        if len(source) > MAX_QUERY:
            return
        line, start, previous = 0, 0, 0
        for token in TOKENS.finditer(source):
            between = source[previous : token.start()]
            line += between.count("\n")
            if "\n" in between:
                start = token.start() - len(between.rsplit("\n", 1)[1])
            value = token.group()
            style = (
                "italic #94a3b8"
                if value.startswith(("--", "/*"))
                else "#86efac"
                if value.startswith(('"', "'"))
                else "#f9a8d4"
                if value.startswith("$")
                else "#93c5fd"
                if value.upper() in KEYWORDS
                else "#fcd34d"
                if value[0].isdigit() or value[0] in "-."
                else None
            )
            location = token.start()
            for part in value.split("\n"):
                if style:
                    self.spans.setdefault(line, []).append(
                        (location - start, location - start + len(part), style)
                    )
                location += len(part) + 1
                if location <= token.end():
                    line += 1
                    start = location
            previous = token.end()
        self._invalidate()

    def set_diagnostic(self, diagnostic):
        self.diagnostic = diagnostic
        self._invalidate()

    def _invalidate(self):
        # Textual 0.86 has no line cache; newer releases cache rendered strips.
        cache = getattr(self, "_line_cache", None)
        if cache is not None:
            cache.clear()
        self.refresh()

    def get_line(self, line_index):
        text = super().get_line(line_index)
        for start, end, style in self.spans.get(line_index, []):
            text.stylize(style, start, end)
        if self.diagnostic and self.diagnostic.get("line") == line_index + 1:
            column = max(0, min(len(text) - 1, (self.diagnostic.get("column") or 1) - 1))
            text.stylize("underline red", column, column + 1)
        return text
