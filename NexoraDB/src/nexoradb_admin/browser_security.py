"""Browser controls for the bundled admin console, without cookie authentication."""
from __future__ import annotations

import re
from urllib.parse import urlsplit

from starlette.datastructures import MutableHeaders


class BrowserSecurityMiddleware:
    def __init__(self, app, *, production: bool, allowed_origins=()):
        self.app = app
        self.production = production
        origins = []
        for origin in allowed_origins:
            parsed = urlsplit(origin)
            if (parsed.scheme in ({"https"} if production else {"http", "https"})
                    and parsed.netloc and not parsed.username and not parsed.password
                    and parsed.path in ("", "/") and not parsed.query and not parsed.fragment):
                # Reconstruct instead of interpolating unvalidated CSP syntax.
                host = parsed.hostname or ""
                if not re.fullmatch(r"[a-zA-Z0-9.\-:\[\]]+", host):
                    continue
                authority = f"[{host}]" if ":" in host else host
                if parsed.port:
                    authority += f":{parsed.port}"
                origins.append(f"{parsed.scheme}://{authority}")
                origins.append(f"{'wss' if parsed.scheme == 'https' else 'ws'}://{authority}")
        self.csp = (
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data:; font-src 'self'; connect-src 'self' "
            + " ".join(sorted(set(origins)))
            + "; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"
        )

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)

        async def secure_send(message):
            if message["type"] == "http.response.start":
                headers = MutableHeaders(scope=message)
                headers["X-Content-Type-Options"] = "nosniff"
                headers["Referrer-Policy"] = "no-referrer"
                headers["X-Frame-Options"] = "DENY"
                headers["Permissions-Policy"] = "camera=(), microphone=(), geolocation=()"
                path = scope.get("path", "")
                if path == "/admin" or path.startswith("/admin/"):
                    headers["Content-Security-Policy"] = self.csp
                hashed_asset = re.fullmatch(r"/admin/assets/[^/]+-[a-zA-Z0-9_-]{8,}\.(js|css|png|svg|webp|woff2)", path)
                if hashed_asset and message["status"] == 200:
                    headers["Cache-Control"] = "public, max-age=31536000, immutable"
                else:
                    headers["Cache-Control"] = "no-store"
                if self.production and scope.get("scheme") == "https":
                    headers["Strict-Transport-Security"] = "max-age=31536000"
            await send(message)

        await self.app(scope, receive, secure_send)
