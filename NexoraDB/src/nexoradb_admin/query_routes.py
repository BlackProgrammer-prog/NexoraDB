"""Admin editor tools, streaming programs and explicit server cursor leases."""
from contextlib import asynccontextmanager
import hashlib

import anyio
from fastapi import APIRouter, Depends, HTTPException, Request, Response
from pydantic import BaseModel, Field

from .cursor import stream_cursor
from .execution_control import Cancellation, ExecutionStopped, Overloaded, current_execution
from .query_cursors import QueryCursorStore
from .query_runner import QueryExecuteRequest
from .query_tools import prepare_program, validate_query, READ_ONLY
from .query_program import ProgramStream, ProgramResults, needs_graph_manager, validate_graph_program
from .security import TokenError, decode_access_token
from .service import require_active_admin


class OpenRequest(QueryExecuteRequest):
    pageSize: int = Field(100, ge=1, le=500)


class NextRequest(BaseModel):
    sequence: int = Field(ge=2)
    pageSize: int = Field(100, ge=1, le=500)


class AdmittedStream:
    def __init__(self, stream, permit):
        self.stream, self.permit, self.closed = stream, permit, False

    def __next__(self):
        return next(self.stream)

    def __getattr__(self, name):
        return getattr(self.stream, name)

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            self.stream.close()
        finally:
            self.permit.close()


def install_query_routes(app, require_admin, get_engine, get_graph_manager=lambda: None):
    router = APIRouter(prefix='/query', tags=['Query editor'])
    store = QueryCursorStore()
    stream_budget = store.budget  # Streaming and idle managed leases share one admin quota.
    app.state.query_cursors = store
    app.state.lifecycle.resource_closers.append(store.shutdown)
    results = ProgramResults(app.state.settings.db_path / 'query_requests.sqlite3')
    app.state.lifecycle.resource_closers.append(results.shutdown)
    app.state.query_program_results = results
    controller = app.state.lifecycle.controller
    # Preserve the owner's reserved small-work cost as well as global capacity.
    large_cost = controller.policy.owner_cost - (controller.policy.small_cost if controller.policy.small_slots else 0)

    def program(payload, request, owner, authorize):
        statements, semantic = prepare_program(payload)
        engine = authorize()
        graph = get_graph_manager() if needs_graph_manager(statements) else None
        validate_graph_program(statements, graph)
        def factory():
            return ProgramStream(engine, statements, semantic, graph, authorize)
        if any(type(s).__name__ not in READ_ONLY for s in statements):
            return results.prepare(owner, request.headers.get('Idempotency-Key'),
                                   {'query': payload.query, 'parameters': payload.parameters}, factory)
        return factory()

    def identity(request, access):
        header = request.headers.get('authorization', '')
        owner = 'admin:' + str(access['sub'])
        session = hashlib.sha256(header.encode()).hexdigest()
        def authorize():
            try:
                payload = decode_access_token(header.removeprefix('Bearer ').strip(), app.state.settings)
            except TokenError as exc:
                raise HTTPException(401, 'Session expired') from exc
            require_active_admin(get_engine(), payload)
            return get_engine()
        return owner, session, authorize

    def guarded(operation, owner, cost=1):
        token = current_execution.get() or Cancellation()
        try:
            with controller.acquire(owner, cost, cancellation=token):
                return operation()
        except ExecutionStopped as exc:
            raise HTTPException(408, 'Execution expired or cancelled; reconcile the request key before retrying writes') from exc
        except Overloaded as exc:
            raise HTTPException(503, 'Query capacity exhausted', headers={'Retry-After': '1'}) from exc
        except RuntimeError as exc:
            raise HTTPException(400, 'Execution failed; earlier writes may have committed. Reconcile the request key before retrying') from exc

    @router.post('/validate')
    def validate(payload: QueryExecuteRequest, access=Depends(require_admin)):
        cost = min(large_cost, 1 + len(payload.query.encode()) // 16384)
        return guarded(lambda: validate_query(payload), 'admin:' + str(access['sub']), cost)

    @router.get('/requests/{request_key}')
    def request_status(request_key: str, access=Depends(require_admin)):
        return results.journal.inspect('admin:' + str(access['sub']), request_key)

    @router.post('/cursors')
    def open_cursor(payload: OpenRequest, request: Request, access=Depends(require_admin)):
        owner, session, authorize = identity(request, access)
        def execute():
            return store.open(owner, session, authorize(), None, None, authorize, payload.pageSize,
                              stream_factory=lambda: program(payload, request, owner, authorize))
        return guarded(execute, owner, large_cost)

    @router.post('/cursors/{cursor_id}/next')
    def next_page(cursor_id: str, payload: NextRequest, request: Request, access=Depends(require_admin)):
        owner, session, authorize = identity(request, access)
        return guarded(lambda: store.page(cursor_id, owner, session, payload.sequence, payload.pageSize, authorize), owner, large_cost)

    @router.delete('/cursors/{cursor_id}', status_code=204)
    def close_cursor(cursor_id: str, request: Request, access=Depends(require_admin)):
        owner, session, _ = identity(request, access)
        store.close(cursor_id, owner, session)
        return Response(status_code=204)

    @router.post('/stream')
    async def stream_query(payload: OpenRequest, request: Request, access=Depends(require_admin)):
        owner, _, authorize = identity(request, access)
        def open_stream():
            token = current_execution.get() or Cancellation()
            # A bounded output page can still scan a large collection. Do not
            # classify it as cheap work and consume the protected small-query share.
            permit = controller.acquire(owner, large_cost, cancellation=token)
            try:
                native = program(payload, request, owner, authorize)
                return AdmittedStream(native, permit)
            except BaseException:
                permit.close()
                raise
        try:
            return await stream_cursor(open_stream, authorize, owner, stream_budget,
                                       batch_size=payload.pageSize)
        except ExecutionStopped as exc:
            raise HTTPException(408, 'Streaming execution expired or cancelled; reconcile the request key before retrying writes') from exc
        except Overloaded as exc:
            raise HTTPException(503, 'Query capacity exhausted', headers={'Retry-After': '1'}) from exc
        except RuntimeError as exc:
            raise HTTPException(400, 'Unable to open results; earlier writes may have committed. Reconcile the request key before retrying') from exc

    app.include_router(router)
    previous = app.router.lifespan_context

    @asynccontextmanager
    async def lifespan(application):
        async with previous(application):
            async with anyio.create_task_group() as tasks:
                async def reap():
                    while True:
                        await anyio.sleep(1)
                        await anyio.to_thread.run_sync(store.reap)
                        await anyio.to_thread.run_sync(results.reap)
                tasks.start_soon(reap)
                try:
                    yield
                finally:
                    tasks.cancel_scope.cancel()
    app.router.lifespan_context = lifespan
