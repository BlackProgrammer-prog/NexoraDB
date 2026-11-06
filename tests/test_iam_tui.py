import asyncio
import sys
from pathlib import Path

from textual.app import App, ComposeResult
from textual.widgets import Button, Input, Static

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "NexoraDB/src"))
from nexoradb.cli.security_panel import SecurityPanel  # noqa: E402


def test_security_panel_keeps_reset_factors_out_of_logs_and_clears_them(monkeypatch):
    calls = []
    factor = "nxreset_" + "a" * 64

    class Api:
        def __init__(self, *_):
            self.client = self

        async def request(self, method, path, payload=None, **_):
            calls.append((method, path, payload))
            return {"resetToken": factor}

        async def close(self):
            pass

    monkeypatch.setattr("nexoradb.cli.security_panel.ConsoleApi", Api)

    class Console(App):
        def compose(self) -> ComposeResult:
            yield SecurityPanel("http://localhost:8000", "session", is_superuser=True)

    async def verify():
        app = Console()
        async with app.run_test(size=(120, 65)) as pilot:
            panel = app.query_one(SecurityPanel)
            panel.query_one("#security-id", Input).value = "b" * 64
            panel.query_one("#security-reset", Button).press()
            await pilot.pause(0.2)
            assert calls == [("POST", "/security/users/" + "b" * 64 + "/reset", {})]
            field = panel.query_one("#security-factor", Input)
            assert field.password and field.value == factor
            assert factor not in str(
                panel.query_one("#security-result", Static).render()
            )
            panel._hide_factor()
            assert field.password and field.value == ""

    asyncio.run(verify())


def test_security_panel_admin_visibility_and_atomic_creation(monkeypatch):
    from textual.widgets import Checkbox

    calls = []

    class Api:
        def __init__(self, *_):
            self.client = self

        async def request(self, method, path, payload=None, **_):
            calls.append((method, path, payload))
            return {}

        async def close(self):
            pass

    monkeypatch.setattr("nexoradb.cli.security_panel.ConsoleApi", Api)

    class Console(App):
        def __init__(self, admin, owner):
            super().__init__()
            self.admin, self.owner = admin, owner

        def compose(self):
            yield SecurityPanel(
                "http://localhost:8000",
                "session",
                is_superuser=self.admin,
                protected_root=self.owner,
            )

    async def verify():
        async with Console(False, False).run_test() as pilot:
            panel = pilot.app.query_one(SecurityPanel)
            assert not panel.query_one("#security-admin").display
            assert panel.query_one("#security-superuser", Checkbox).disabled
            panel.query_one("#security-create", Button).press()
            await pilot.pause()
            assert not calls
        async with Console(True, False).run_test() as pilot:
            panel = pilot.app.query_one(SecurityPanel)
            assert panel.query_one("#security-admin").display
            assert panel.query_one("#security-superuser", Checkbox).disabled
        async with Console(True, True).run_test() as pilot:
            panel = pilot.app.query_one(SecurityPanel)
            panel.query_one("#security-username", Input).value = "operator"
            panel.query_one("#security-initial", Input).value = "initial password test"
            panel.query_one("#security-grants", Input).value = '{"posts":["read"]}'
            panel.query_one("#security-superuser", Checkbox).value = True
            panel.query_one("#security-create", Button).press()
            await pilot.pause(0.2)
            assert calls == [
                (
                    "POST",
                    "/security/users",
                    {
                        "username": "operator",
                        "password": "initial password test",
                        "roles": ["superuser"],
                        "grants": {"posts": ["read"]},
                    },
                )
            ]
            assert panel.query_one("#security-initial", Input).value == ""

    asyncio.run(verify())
