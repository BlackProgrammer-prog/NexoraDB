"""Async terminal query transport; the admin API owns execution semantics.

No automatic retries, redirects, cookies, environment proxies or persisted tokens.
Cancelling the asyncio task closes its HTTP response; it does not undo writes.
"""

from __future__ import annotations

import asyncio
import json
import math
import re
from urllib.parse import quote

import httpx

MAX_RESPONSE = 8 * 1024 * 1024
MAX_FRAME = 2 * 1024 * 1024


class QueryClientError(Exception):
    def __init__(self, message, *, status=None, details=None):
        super().__init__(message)
        self.status, self.details = status, details


def strict_json(value):
    def invalid(constant):
        raise ValueError("Non-finite JSON numbers are unsupported")

    def real(value):
        result = float(value)
        if not math.isfinite(result):
            invalid(value)
        return result

    return json.loads(value, parse_constant=invalid, parse_float=real)


class QueryClient:
    def __init__(self, base_url, token=None, *, transport=None):
        url = httpx.URL(base_url)
        if (
            url.scheme not in {"http", "https"}
            or not url.host
            or url.userinfo
            or url.query
            or url.fragment
        ):
            raise ValueError(
                "Expected an HTTP(S) server URL without credentials, query or fragment"
            )

        async def no_cookies(request):
            request.headers.pop("cookie", None)

        self.client = httpx.AsyncClient(
            base_url=base_url.rstrip("/") + "/",
            headers={
                **({"Authorization": "Bearer " + token} if token else {}),
                "Accept-Encoding": "identity",
            },
            timeout=httpx.Timeout(35, connect=5, pool=5),
            follow_redirects=False,
            trust_env=False,
            transport=transport,
            event_hooks={"request": [no_cookies]},
        )

    async def close(self):
        await self.client.aclose()
        self.client.headers.pop("authorization", None)
        self.client.cookies.clear()

    def _body(self, payload):
        body = json.dumps(
            payload, ensure_ascii=False, allow_nan=False, separators=(",", ":")
        ).encode()
        if len(body) > 1024 * 1024:
            raise QueryClientError("Request exceeds 1 MiB; narrow the query or parameters")
        return body

    async def _read(self, response, limit):
        data = bytearray()
        async for chunk in response.aiter_bytes():
            if len(data) + len(chunk) > limit:
                raise QueryClientError(
                    "Response exceeds the terminal byte budget; use streaming or narrow the query"
                )
            data.extend(chunk)
        return bytes(data)

    def _encoding(self, response):
        if response.headers.get("content-encoding", "identity").lower() != "identity":
            raise QueryClientError("Unexpected compressed response")

    async def _check(self, response):
        self._encoding(response)
        if not response.is_success:
            raw = await self._read(response, 64 * 1024)
            details = None
            try:
                details = strict_json(raw)
                message = details.get("message") or details.get("detail")
                if isinstance(message, dict):
                    message = message.get("message")
            except (ValueError, AttributeError):
                message = None
            # Redirect bodies/locations and non-JSON gateway pages are not displayed.
            raise QueryClientError(
                message
                if isinstance(message, str)
                else f"Query API failed ({response.status_code})",
                status=response.status_code,
                details=details,
            )

    async def request(self, method, path, payload=None, *, key=None, timeout=35, allow_list=False):
        if not path.startswith("/") or path.startswith("//") or "\\" in path:
            raise QueryClientError("Invalid API path")
        try:
            return await asyncio.wait_for(
                self._request(
                    method, path, payload, key=key, timeout=timeout, allow_list=allow_list
                ),
                timeout,
            )
        except asyncio.TimeoutError as exc:
            raise QueryClientError(
                "Query deadline exceeded; reconcile the request key before retrying writes"
            ) from exc

    async def _request(self, method, path, payload=None, *, key=None, timeout=35, allow_list=False):
        headers = {"Content-Type": "application/json"}
        if key:
            headers["Idempotency-Key"] = key
        try:
            async with self.client.stream(
                method,
                path.lstrip("/"),
                content=self._body(payload) if payload is not None else None,
                headers=headers,
                timeout=timeout,
            ) as response:
                await self._check(response)
                if response.status_code == 204:
                    return {}
                if "application/json" not in response.headers.get("content-type", "").lower():
                    raise QueryClientError("Expected a JSON response")
                result = strict_json(await self._read(response, MAX_RESPONSE))
                if not isinstance(result, dict) and not (allow_list and isinstance(result, list)):
                    raise QueryClientError("Invalid query API response")
                return result
        except (httpx.HTTPError, ValueError, RecursionError) as exc:
            raise QueryClientError(
                "Query transport failed; reconcile the request key before retrying writes"
            ) from exc

    async def validate(self, query, parameters):
        result = await self.request(
            "POST", "/query/validate", {"query": query, "parameters": parameters}
        )
        diagnostics = result.get("diagnostics")
        if (
            type(result.get("valid")) is not bool
            or type(result.get("cursorSupported")) is not bool
            or not isinstance(diagnostics, list)
            or len(diagnostics) > 256
            or any(
                not isinstance(item, dict)
                or not isinstance(item.get("message"), str)
                or any(
                    item.get(field) is not None
                    and (type(item[field]) is not int or item[field] < 1)
                    for field in ("line", "column")
                )
                for item in diagnostics
            )
        ):
            raise QueryClientError("Invalid diagnostics response")
        if not result["valid"] and not diagnostics:
            raise QueryClientError("Validation failed without diagnostics")
        return result

    async def buffered(self, query, parameters):
        return await self.request(
            "POST",
            "/query/execute",
            {"query": query, "parameters": parameters, "include_raw": False},
        )

    async def open_cursor(self, query, parameters, key):
        return self._page(
            await self.request(
                "POST",
                "/query/cursors",
                {"query": query, "parameters": parameters, "pageSize": 100},
                key=key,
            ),
            1,
        )

    async def next_page(self, identifier, sequence):
        self._identifier(identifier)
        return self._page(
            await self.request(
                "POST",
                "/query/cursors/" + quote(identifier, safe="") + "/next",
                {"sequence": sequence, "pageSize": 100},
            ),
            sequence,
        )

    async def close_cursor(self, identifier):
        self._identifier(identifier)
        return await self.request(
            "DELETE", "/query/cursors/" + quote(identifier, safe=""), timeout=3
        )

    async def receipt(self, key):
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", key):
            raise QueryClientError("Invalid request key")
        return await self.request(
            "GET", "/query/requests/" + quote(key, safe="").replace(".", "%2E")
        )

    @staticmethod
    def _identifier(identifier):
        if not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", identifier):
            raise QueryClientError("Invalid server cursor identifier")

    def _page(self, page, sequence):
        identifier = page.get("cursorId")
        if identifier is not None:
            self._identifier(identifier)
        rows = page.get("rows")
        valid = (
            type(page.get("sequence")) is int
            and page["sequence"] == sequence
            and type(page.get("totalRows")) is int
            and isinstance(rows, list)
            and len(rows) <= 100
            and all(isinstance(row, dict) for row in rows)
            and page["totalRows"] >= len(rows)
            and type(page.get("hasMore")) is bool
            and page["hasMore"] == bool(identifier)
            and isinstance(page.get("statementErrors", []), list)
            and all(isinstance(error, str) for error in page.get("statementErrors", []))
        )
        if not valid:
            raise QueryClientError(
                "Invalid cursor page; no page was retried", details={"cursorId": identifier}
            )
        return page

    async def stream(self, query, parameters, key, consume):
        """Await each consumer batch: UI backpressure, not an unbounded event queue."""
        try:
            async with self.client.stream(
                "POST",
                "query/stream",
                headers={"Content-Type": "application/json", "Idempotency-Key": key},
                content=self._body({"query": query, "parameters": parameters, "pageSize": 128}),
                timeout=120,
            ) as response:
                await self._check(response)
                if "application/x-ndjson" not in response.headers.get("content-type", "").lower():
                    raise QueryClientError("Expected an NDJSON stream")
                pending, batch, ended = bytearray(), [], False
                batch_bytes = 0
                async for chunk in response.aiter_bytes():
                    # Split by frame boundaries without buffering a complete response.
                    offset = 0
                    while offset < len(chunk):
                        boundary = chunk.find(b"\n", offset)
                        end = len(chunk) if boundary < 0 else boundary
                        if len(pending) + end - offset > MAX_FRAME:
                            raise QueryClientError("Stream record exceeds 2 MiB")
                        pending.extend(chunk[offset:end])
                        offset = end + 1
                        if boundary < 0:
                            break
                        if not pending.strip():
                            pending.clear()
                            continue
                        if ended:
                            raise QueryClientError("Unexpected data after stream completion")
                        frame_bytes = len(pending)
                        frame = strict_json(pending)
                        pending.clear()
                        if not isinstance(frame, dict):
                            raise QueryClientError("Invalid stream frame")
                        if isinstance(frame.get("error"), str):
                            if batch:
                                await consume(batch)
                            raise QueryClientError(frame["error"])
                        if frame.get("end") is True:
                            if "document" in frame:
                                raise QueryClientError("Invalid completion frame")
                            ended = True
                        elif isinstance(frame.get("document"), dict):
                            batch.append(frame["document"])
                            batch_bytes += frame_bytes
                            if len(batch) == 128 or batch_bytes >= 1024 * 1024:
                                await consume(batch)
                                batch = []
                                batch_bytes = 0
                        else:
                            raise QueryClientError("Invalid stream row")
                    if batch:
                        await consume(batch)
                        batch = []
                        batch_bytes = 0
                if pending.strip() or not ended:
                    raise QueryClientError(
                        "Stream ended without completion; results are incomplete"
                    )
        except (httpx.HTTPError, ValueError, RecursionError) as exc:
            raise QueryClientError(
                "Stream transport failed; preview is incomplete. Reconcile writes before retrying"
            ) from exc
