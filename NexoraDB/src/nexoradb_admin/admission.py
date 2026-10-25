"""Per-process HTTP admission, held until the complete response/disconnect.

No waiting queue: excess requests are rejected before auth/body/query work.
The counter uses a lock so TestClient/multiple loop callers are safe as well.
"""
from threading import Lock
import anyio

from starlette.responses import JSONResponse


class AdmissionMiddleware:
    def __init__(self, app, max_inflight: int = 16, max_body_bytes: int = 2 * 1024 * 1024,
                 require_https: bool = False):
        self.app = app
        self.limit = max_inflight
        self.active = 0
        self.lock = Lock()
        self.max_body_bytes = max_body_bytes
        self.require_https = require_https

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        if self.require_https and scope.get("scheme") != "https":
            return await JSONResponse({"message": "HTTPS required"}, status_code=400)(scope, receive, send)
        with self.lock:
            accepted = self.active < self.limit
            if accepted:
                self.active += 1
        if not accepted:
            response = JSONResponse(
                {"message": "API capacity exhausted; retry later"},
                status_code=503, headers={"Retry-After": "1"},
            )
            return await response(scope, receive, send)
        try:
            # Bound the body before downstream JSON parsing. Check actual
            # bytes, including chunked requests, not just Content-Length.
            # A list of tiny/empty network fragments can consume far more
            # memory than the byte limit. Keep one bounded contiguous buffer.
            buffered = bytearray()
            total = 0
            with anyio.move_on_after(10) as body_timeout:
                while True:
                    message = await receive()
                    if message["type"] == "http.disconnect":
                        return
                    body = message.get("body", b"")
                    total += len(body)
                    if total > self.max_body_bytes:
                        return await JSONResponse({"message": "Request body too large"}, status_code=413)(scope, receive, send)
                    buffered.extend(body)
                    if not message.get("more_body", False):
                        break
            if body_timeout.cancelled_caught:
                return await JSONResponse({"message": "Request body timeout"}, status_code=408)(scope, receive, send)
            delivered = False
            async def bounded_receive():
                nonlocal delivered
                if not delivered:
                    delivered = True
                    body = bytes(buffered)
                    buffered.clear()
                    return {"type": "http.request", "body": body, "more_body": False}
                return await receive()
            async def secure_send(message):
                if message["type"] == "http.response.start":
                    headers = list(message.get("headers", []))
                    if not any(name.lower() == b"cache-control" for name, _ in headers):
                        headers.append((b"cache-control", b"no-store"))
                    headers.append((b"x-content-type-options", b"nosniff"))
                    message = dict(message, headers=headers)
                await send(message)
            await self.app(scope, bounded_receive, secure_send)
        finally:
            with self.lock:
                self.active -= 1
