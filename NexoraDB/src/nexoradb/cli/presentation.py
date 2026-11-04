"""Literal, bounded terminal presentation; never interpret data as Rich markup."""

import json

from rich.text import Text
from textual.binding import Binding
from textual.widgets import Input

from .query_editor import safe_text


def literal(value, limit=4000, style=None):
    return Text(safe_text(str(value), limit), style=style)


def json_preview(value, limit=16384):
    chunks, size, clipped = [], 0, False
    for chunk in json.JSONEncoder(ensure_ascii=False, allow_nan=False).iterencode(value):
        left = limit - size
        chunks.append(chunk[:left])
        size += min(left, len(chunk))
        if len(chunk) > left:
            clipped = True
            break
    text = "".join(chunks)
    if clipped:
        text += "\n[Preview truncated; use the query workspace for detailed results.]"
    return literal(text, limit + 100)


class PrivateInput(Input):
    BINDINGS = [
        Binding("ctrl+c,super+c", "copy", show=False, priority=True),
        Binding("ctrl+x,super+x", "cut", show=False, priority=True),
    ]

    def action_copy(self):
        if not self.password:
            value = getattr(self, "selected_text", self.value)
            if value:
                self.app.copy_to_clipboard(value)

    def action_cut(self):
        if not self.password:
            action = getattr(super(), "action_cut", None)
            if action:
                action()


class SecretInput(PrivateInput):
    """Read-only issued credential. Copy requires an explicit temporary reveal."""

    async def _on_key(self, event):
        editing = {
            "backspace",
            "delete",
            "ctrl+u",
            "ctrl+k",
            "ctrl+w",
            "ctrl+v",
            "super+v",
            "ctrl+backspace",
            "ctrl+delete",
            "alt+backspace",
            "alt+delete",
        }
        if event.is_printable or event.key in editing:
            event.stop()
            event.prevent_default()
            return
        await super()._on_key(event)

    def _on_paste(self, event):
        event.stop()
        event.prevent_default()

    def action_cut(self):
        pass

    def action_paste(self):
        pass
