"""Bounded live HTTP cursors. No unbounded result registry or materialization."""
import json
import time
from threading import Lock

import anyio
from fastapi import HTTPException
from fastapi.responses import StreamingResponse


class CursorBudget:
    def __init__(self, total=8, per_owner=2):
        self.total, self.per_owner = total, per_owner
        self.owners = {}
        self.lock = Lock()

    def acquire(self, owner):
        with self.lock:
            if sum(self.owners.values()) >= self.total or self.owners.get(owner, 0) >= self.per_owner:
                raise HTTPException(429, "Cursor capacity exhausted", headers={"Retry-After": "1"})
            self.owners[owner] = self.owners.get(owner, 0) + 1

    def release(self, owner):
        with self.lock:
            count = self.owners.get(owner, 0)
            if count <= 1:
                self.owners.pop(owner, None)
            else:
                self.owners[owner] = count - 1


async def stream_cursor(open_stream, authorize, owner, budget, *, batch_size=128,
                        limit=None, skip=0, project=None, max_seconds=120):
    budget.acquire(owner)
    stream = None
    closed = False
    started = time.monotonic()
    remaining = limit
    to_skip = skip
    sentinel = object()
    raw_batch = None

    def take():
        nonlocal remaining, to_skip
        authorize()  # Expiry, revocation, resource permission on every batch.
        if time.monotonic() - started > max_seconds:
            raise RuntimeError("cursor deadline exceeded")
        if raw_batch is not None:
            if remaining == 0:
                return b""
            chunk, count = raw_batch(min(batch_size, remaining) if remaining is not None else batch_size)
            if remaining is not None:
                remaining -= count
            return chunk
        while to_skip:
            if next(stream, sentinel) is sentinel:
                return ""
            to_skip -= 1
            if time.monotonic() - started > max_seconds:
                raise RuntimeError("cursor deadline exceeded")
        lines = []
        encoded_bytes = 0
        for _ in range(batch_size):
            if remaining == 0:
                break
            item = next(stream, sentinel)
            if item is sentinel:
                break
            if project:
                item = project(item)
            line = (json.dumps({"document": item}, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
            if len(line) > 2 * 1024 * 1024:
                raise RuntimeError("cursor record exceeds 2MiB")
            lines.append(line)
            encoded_bytes += len(line)
            if remaining is not None:
                remaining -= 1
            if encoded_bytes >= 1024 * 1024:
                break
        return b"".join(lines)

    async def cleanup():
        nonlocal closed
        if closed:
            return
        closed = True
        try:
            if stream is not None:
                with anyio.CancelScope(shield=True):
                    await anyio.to_thread.run_sync(stream.close)
        finally:
            budget.release(owner)

    try:
        stream = await anyio.to_thread.run_sync(open_stream)
        # Only native streams without projection/offset can use this path.
        # Auth/deadline checks and send backpressure still apply per batch.
        if project is None and skip == 0:
            raw_batch = getattr(stream, "next_ndjson_batch", None)
        first = await anyio.to_thread.run_sync(take)
    except BaseException:
        await cleanup()
        raise

    async def generate():
        try:
            chunk = first
            while chunk:
                yield chunk
                chunk = await anyio.to_thread.run_sync(take)
            yield '{"end":true}\n'
        except Exception:
            yield '{"error":"cursor failed; discard incomplete result"}\n'
        finally:
            await cleanup()

    class Response(StreamingResponse):
        async def __call__(self, scope, receive, send):
            try:
                # Bounds slow consumers and time retaining RocksDB snapshots.
                with anyio.fail_after(max(0.001, max_seconds - (time.monotonic() - started))):
                    await super().__call__(scope, receive, send)
            finally:
                await cleanup()

    return Response(generate(), media_type="application/x-ndjson",
                    headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"})
