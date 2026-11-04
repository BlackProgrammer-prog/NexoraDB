"""Process-local native cursor leases: bounded, session-bound, sequential pages."""
from dataclasses import dataclass, field
import json
import secrets
from threading import Lock, RLock
import time

from fastapi import HTTPException

from .cursor import CursorBudget
from .execution_control import Cancellation, current_execution


@dataclass
class Lease:
    owner: str
    session: str
    stream: object
    project: object
    skip: int
    remaining: int | None
    control: Cancellation
    created: float
    touched: float
    sequence: int = 0
    count: int = 0
    closed: bool = False
    lock: object = field(default_factory=Lock)


class QueryCursorStore:
    def __init__(self, *, idle_seconds=30, max_seconds=120, total=8, per_owner=2, clock=time.monotonic):
        self.clock, self.idle_seconds, self.max_seconds = clock, idle_seconds, max_seconds
        self.entries = {}
        self.lock = RLock()
        self.budget = CursorBudget(total, per_owner)
        self.stopping = False

    def open(self, owner, session, engine, statement, semantic, authorize, page_size, stream_factory=None):
        self.reap()
        self.budget.acquire(owner)
        stream = None
        try:
            control = Cancellation(self.max_seconds)
            factory = getattr(engine, 'execution_scope', None)
            if stream_factory is not None:
                stream = stream_factory()
                if getattr(stream, 'scope', None) is not None:
                    control.attach(stream.scope)
            elif factory:
                scope = factory(int(self.max_seconds * 1000))
                control.attach(scope)
                with scope:
                    stream = engine.iter_documents(statement.collection, semantic.build_condition(statement.where),
                                                   page_size=page_size, max_page_bytes=1024 * 1024)
            else:
                stream = engine.iter_documents(statement.collection, semantic.build_condition(statement.where),
                                               page_size=page_size, max_page_bytes=1024 * 1024)
            now = self.clock()
            entry = Lease(owner, session, stream,
                          (lambda row: row) if stream_factory else semantic.Executor._compile_projection(statement.projection),
                          0 if stream_factory else statement.skip,
                          None if stream_factory else statement.limit or None, control, now, now)
            cursor_id = secrets.token_urlsafe(24)
            with self.lock:
                if self.stopping:
                    raise HTTPException(503, 'Service stopping')
                self.entries[cursor_id] = entry
        except BaseException:
            try:
                if stream is not None:
                    stream.close()
            finally:
                self.budget.release(owner)
            raise
        return self.page(cursor_id, owner, session, 1, page_size, authorize)

    def _entry(self, cursor_id, owner, session):
        with self.lock:
            entry = self.entries.get(cursor_id)
        if entry is None or entry.owner != owner or entry.session != session:
            raise HTTPException(404, 'Cursor unavailable or expired; rerun the query')
        return entry

    def _finish(self, cursor_id, entry):
        if entry.closed:
            return
        entry.closed = True
        with self.lock:
            self.entries.pop(cursor_id, None)
        try:
            entry.control.cancel('cursor_closed')
            entry.stream.close()
        finally:
            self.budget.release(entry.owner)

    def _expired(self, entry):
        now = self.clock()
        return now - entry.created >= self.max_seconds or now - entry.touched >= self.idle_seconds

    def page(self, cursor_id, owner, session, sequence, page_size, authorize):
        entry = self._entry(cursor_id, owner, session)
        if not entry.lock.acquire(blocking=False):
            raise HTTPException(409, 'Cursor page already in progress')
        try:
            if entry.closed or self._expired(entry):
                raise HTTPException(410, 'Cursor expired; rerun the query')
            if sequence != entry.sequence + 1:
                raise HTTPException(409, 'Cursor sequence mismatch; do not retry a lost page, rerun the query')
            entry.control.check()
            authorize()
            request = current_execution.get()
            if request is not None and entry.control.native is not None:
                request.attach(entry.control.native)
            def check():
                entry.control.check()
                if request is not None:
                    request.check()
            while entry.skip:
                check()
                if entry.skip % 128 == 0:
                    authorize()
                if next(entry.stream, None) is None:
                    entry.remaining = 0
                    break
                entry.skip -= 1
            rows, size = [], 0
            exhausted = entry.remaining == 0
            for _ in range(page_size):
                if exhausted:
                    break
                check()
                if len(rows) % 128 == 0:
                    authorize()
                row = next(entry.stream, None)
                if row is None:
                    exhausted = True
                    break
                row = entry.project(row)
                size += len(json.dumps(row, ensure_ascii=False).encode('utf-8'))
                if size > 2 * 1024 * 1024:
                    raise HTTPException(413, 'Cursor page exceeds 2 MiB; select fewer fields')
                rows.append(row)
                if entry.remaining is not None:
                    entry.remaining -= 1
                    exhausted = entry.remaining == 0
            entry.sequence += 1
            check()
            authorize()
            entry.count += len(rows)
            entry.touched = self.clock()
            if exhausted:
                self._finish(cursor_id, entry)
            columns = {}
            for row in rows:
                for key in row:
                    if len(columns) >= 1000:
                        break
                    columns[key] = None
            return dict(cursorId=None if exhausted else cursor_id, rows=rows,
                        columns=list(columns),
                        statementErrors=list(getattr(entry.stream, 'errors', [])),
                        hasMore=not exhausted, sequence=entry.sequence, totalRows=entry.count,
                        idleTimeoutSeconds=self.idle_seconds, maxLifetimeSeconds=self.max_seconds)
        except HTTPException as exc:
            if exc.status_code != 409:
                self._finish(cursor_id, entry)
            raise
        except BaseException:
            self._finish(cursor_id, entry)
            raise
        finally:
            entry.lock.release()

    def close(self, cursor_id, owner, session):
        with self.lock:
            if cursor_id not in self.entries:
                return
        entry = self._entry(cursor_id, owner, session)
        entry.control.cancel('cursor_closed')
        with entry.lock:
            self._finish(cursor_id, entry)

    def reap(self):
        with self.lock:
            entries = list(self.entries.items())
        for cursor_id, entry in entries:
            if self._expired(entry) and entry.lock.acquire(blocking=False):
                try:
                    if self._expired(entry):
                        self._finish(cursor_id, entry)
                finally:
                    entry.lock.release()

    def shutdown(self):
        with self.lock:
            self.stopping = True
            entries = list(self.entries.items())
        for cursor_id, entry in entries:
            with entry.lock:
                self._finish(cursor_id, entry)
