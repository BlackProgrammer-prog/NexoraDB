"""Credential administration without putting secrets in the query workspace."""

import json
from urllib.parse import quote

from textual.app import ComposeResult
from textual.containers import ScrollableContainer, Vertical
from textual.message import Message
from textual.widgets import Button, Checkbox, Input, Static

from .console_api import ConsoleApi
from .presentation import PrivateInput, SecretInput, literal
from .tasks import ScreenTasks


class SecurityPanel(ScreenTasks, ScrollableContainer):
    DEFAULT_CSS = "SecurityPanel #security-admin { height: auto; }"

    class PasswordChanged(Message):
        pass

    def __init__(self, url, token, *, is_superuser=False, protected_root=False):
        super().__init__()
        self.api = ConsoleApi(url, token)
        self._init_tasks()
        self._factor_timer = None
        self.is_superuser = is_superuser
        self.protected_root = protected_root

    def compose(self) -> ComposeResult:
        yield Static("Account and access", classes="title")
        yield PrivateInput(
            placeholder="Current password", password=True, id="security-current", max_length=1024
        )
        yield PrivateInput(
            placeholder="New password (15+ characters)",
            password=True,
            id="security-new",
            max_length=1024,
        )
        yield Button("Change password and sign out", id="security-change")
        with Vertical(id="security-admin"):
            yield from self._administration()
        yield Static("Owner recovery is offline: stop the service, then use nexora_iam recover.")
        yield Static("", id="security-result", markup=False)

    def on_mount(self):
        self.query_one("#security-admin").display = self.is_superuser

    def _administration(self):
        yield Static(
            "Superuser administration — new users have no implicit access", classes="title"
        )
        yield Input(placeholder="New username", id="security-username", max_length=64)
        yield PrivateInput(
            placeholder="Initial password", password=True, id="security-initial", max_length=1024
        )
        yield Static("The roles and collection grants below apply when creating a user.")
        yield Checkbox(
            "SuperUser (only the initial protected owner can grant)",
            id="security-superuser",
            disabled=not self.protected_root,
        )
        yield Button("Create user with permissions", id="security-create")
        yield Button("List users and immutable IDs", id="security-users")
        yield Input(placeholder="Target identity ID", id="security-id", max_length=64)
        yield Input(
            placeholder='Collection grants JSON, e.g. {"orders":["read"]}',
            value="{}",
            id="security-grants",
            max_length=8192,
        )
        yield Input(
            placeholder="Roles as comma-separated names", id="security-roles", max_length=1024
        )
        yield Button("Replace target roles and grants", id="security-access")
        yield Input(placeholder="Collection name", id="security-collection", max_length=128)
        yield Checkbox("Read", id="security-read")
        yield Checkbox("Write", id="security-write")
        yield Checkbox("Manage schema / indexes", id="security-manage")
        yield Button("Add / replace collection grant", id="security-add-grant")
        yield Static(
            "Only the initial protected owner can grant SuperUser. "
            "Access changes revoke target sessions."
        )
        yield Input(placeholder="Role definition name", id="security-role-name", max_length=64)
        yield Input(
            placeholder="Parent roles, comma-separated (never superuser)",
            id="security-parents",
            max_length=1024,
        )
        yield Button("Save role using collection grants above", id="security-save-role")
        yield Button("Disable target user", id="security-disable")
        yield Button("Enable target user", id="security-enable")
        yield Button("Issue reset token (shown for 60 seconds)", id="security-reset")
        yield SecretInput(placeholder="One-time reset token", password=True, id="security-factor")
        yield Button("Reveal / hide reset token", id="security-reveal")

    def _busy_changed(self, _name, busy):
        for button in self.query(Button):
            button.disabled = busy

    def _task_error(self, _name, _error):
        self.query_one("#security-result", Static).update(
            "Security operation failed or access denied; check the session and input."
        )

    def _value(self, field, clear=False):
        widget = self.query_one("#security-" + field, Input)
        value = widget.value
        if clear:
            widget.value = ""
        return value

    def _hide_factor(self):
        widget = self.query_one("#security-factor", Input)
        widget.value, widget.password = "", True

    def on_button_pressed(self, event: Button.Pressed):
        event.stop()
        action = event.button.id
        if action != "security-change" and not self.is_superuser:
            return
        if action == "security-reveal":
            widget = self.query_one("#security-factor", Input)
            widget.password = not widget.password
            return
        if action == "security-add-grant":
            try:
                grants = json.loads(self._value("grants"))
                collection = self._value("collection").strip()
                if not collection or not isinstance(grants, dict):
                    raise ValueError("Collection required")
                grants[collection] = [
                    name
                    for name in ("read", "write", "manage")
                    if self.query_one("#security-" + name, Checkbox).value
                ]
                self.query_one("#security-grants", Input).value = json.dumps(grants)
            except (ValueError, TypeError):
                self._task_error("security", None)
            return
        self._hide_factor()
        current, replacement = self._value("current", True), self._value("new", True)
        initial = self._value("initial", True)
        identity, name = self._value("id"), self._value("username")
        grants, roles = self._value("grants"), self._value("roles")
        assigned_roles = [part.strip() for part in roles.split(",") if part.strip()]
        if self.protected_root and self.query_one("#security-superuser", Checkbox).value:
            if "superuser" not in assigned_roles:
                assigned_roles.append("superuser")
        if (
            action in {"security-create", "security-access"}
            and not self.protected_root
            and "superuser" in assigned_roles
        ):
            self.query_one("#security-result", Static).update(
                "Only the initial protected owner can grant SuperUser."
            )
            return

        async def execute():
            request = self.api.client.request
            if action == "security-change":
                await request(
                    "POST",
                    "/auth/change-password",
                    {"currentPassword": current, "newPassword": replacement},
                )
                self.post_message(self.PasswordChanged())
            elif action == "security-create":
                await request(
                    "POST",
                    "/security/users",
                    {
                        "username": name,
                        "password": initial,
                        "roles": assigned_roles,
                        "grants": json.loads(grants),
                    },
                )
            elif action == "security-users":
                users = await request("GET", "/security/users", allow_list=True)
                summary = "\n".join(
                    f"{item['username']}  {item['_id']}  {item['status']}  "
                    f"roles={','.join(item.get('roles', []))}"
                    for item in users
                )
                self.query_one("#security-result", Static).update(literal(summary, 16000))
                return
            elif action == "security-access":
                await request(
                    "PATCH",
                    "/security/users/" + identity,
                    {
                        "grants": json.loads(grants),
                        "roles": assigned_roles,
                    },
                )
            elif action == "security-save-role":
                await request(
                    "PUT",
                    "/security/roles/" + quote(self._value("role-name"), safe=""),
                    {
                        "grants": json.loads(grants),
                        "parents": [
                            part.strip()
                            for part in self._value("parents").split(",")
                            if part.strip()
                        ],
                    },
                )
            elif action in {"security-disable", "security-enable"}:
                await request(
                    "PATCH",
                    "/security/users/" + identity,
                    {"status": "disabled" if action == "security-disable" else "active"},
                )
            elif action == "security-reset":
                result = await request("POST", "/security/users/" + identity + "/reset", {})
                self.query_one("#security-factor", Input).value = result["resetToken"]
                if self._factor_timer:
                    self._factor_timer.stop()
                self._factor_timer = self.set_timer(60, self._hide_factor)
            self.query_one("#security-result", Static).update(
                "Completed. Reset tokens must be delivered through a verified channel."
            )

        self._launch("security", execute)

    async def on_unmount(self):
        if self._factor_timer:
            self._factor_timer.stop()
        for widget in self.query(Input):
            widget.value = ""
        await self._close_tasks()
        await self.api.close()
