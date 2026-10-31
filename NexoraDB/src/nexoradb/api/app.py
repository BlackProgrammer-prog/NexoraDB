from __future__ import annotations

from collections.abc import Callable
from typing import Any
import json
import anyio
from threading import RLock

from fastapi import APIRouter, Depends, FastAPI, HTTPException, Query, Request, Header, status

from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import create_doc_engine, create_graph_manager, load_native_module
from nexoradb_admin.query_runner import QueryExecuteRequest, QueryExecuteResponse, execute_query
from nexoradb_admin.query_runner import authorize_collection
from nexoradb._version import __version__

from .security import AppTokenClaims, require_app_token, require_scope
from nexoradb_admin.admission import AdmissionMiddleware
from nexoradb_admin.lifecycle import install_lifecycle
from nexoradb_admin.idempotency import RequestJournal
from nexoradb_admin.cursor import CursorBudget, stream_cursor, open_document_stream
from nexoradb_admin.query_runner import _load_nexoraql_package, _authorize_statements, authorize_collections, _raise_query_error
import importlib
import time


def create_api_router(
    *,
    engine_provider: Callable[[], Any],
    graph_manager_provider: Callable[[], Any | None],
) -> APIRouter:
    router = APIRouter(prefix="/api/v1", tags=["NexoraDB API"])
    cursor_budget = CursorBudget()

    def authorize_read(app: AppTokenClaims, collection: str) -> Any:
        require_scope(app, "documents:read")
        authorize_collection(getattr(app, "collections", None), collection, "read")
        engine = engine_provider()
        if app.managed and not engine.is_internal_app_token_active(app.token_id):
            raise HTTPException(status_code=401, detail="application token has been revoked")
        return engine

    def authorize_cursor(app, collection):
        expires_at = getattr(app, "expires_at", None)
        if expires_at is not None and expires_at <= int(time.time()):
            raise HTTPException(401, "Application token expired")
        return authorize_read(app, collection)

    @router.post("/query/cursor")
    async def query_cursor(payload: QueryExecuteRequest,
                           batch_size: int = Query(128, ge=1, le=1000),
                           app: AppTokenClaims = Depends(require_app_token)):
        require_scope(app, "query:execute")
        def prepare():
            parser = _load_nexoraql_package()
            statements = parser.parse(payload.query, payload.parameters)
            if len(statements) != 1 or type(statements[0]).__name__ != "Select":
                raise HTTPException(422, "Cursor requires exactly one SELECT")
            statement = statements[0]
            if statement.joins:
                raise HTTPException(422, "Streaming JOIN cursor is not supported; no buffered fallback")
            _authorize_statements(statements, frozenset(app.scopes))
            authorize_collections(statements, getattr(app, "collections", None))
            semantic = importlib.import_module(parser.__name__ + ".semantic")
            semantic.Validator().validate(statement)
            return statement, semantic
        try:
            statement, semantic = await anyio.to_thread.run_sync(prepare)
        except Exception as exc:
            _raise_query_error(exc, _load_nexoraql_package())
        def authorize():
            return authorize_cursor(app, statement.collection)
        def open_stream():
            return open_document_stream(authorize(), statement.collection, semantic.build_condition(statement.where),
                                              page_size=batch_size, max_page_bytes=1024 * 1024)
        owner = app.app_id
        return await stream_cursor(open_stream, authorize, owner, cursor_budget, batch_size=batch_size,
                                   skip=statement.skip, limit=statement.limit or None,
                                   project=(None if statement.projection is None else
                                            semantic.Executor._compile_projection(statement.projection)))

    @router.get("/collections/{collection}/documents")
    def document_page(
        collection: str, limit: int = Query(100, ge=1, le=1000),
        skip: int = Query(0, ge=0, le=1_000_000),
        app: AppTokenClaims = Depends(require_app_token),
    ):
        # Independent snapshot per request. Bound native pages BEFORE Python
        # decoding. Deep offset requests remain O(skip); prefer /stream.
        engine = authorize_read(app, collection)
        cursor_budget.acquire(app.app_id)
        stream = None
        rows, encoded_bytes = [], 2
        sentinel = object()
        started = time.monotonic()
        try:
            stream = open_document_stream(engine, collection, page_size=min(limit, 128),
                                           max_page_bytes=1024 * 1024)
            for position in range(skip + limit):
                if position % 128 == 0:
                    authorize_cursor(app, collection)
                    if time.monotonic() - started > 30:
                        raise HTTPException(408, "Page deadline exceeded; use streaming cursor")
                row = next(stream, sentinel)
                if row is sentinel:
                    break
                if position < skip:
                    continue
                encoded_bytes += len(json.dumps(row, ensure_ascii=False).encode("utf-8")) + 1
                if encoded_bytes > 4 * 1024 * 1024:
                    raise HTTPException(413, "Page exceeds 4MiB; use document streaming")
                rows.append(row)
        except RuntimeError as exc:
            if "byte budget" in str(exc):
                raise HTTPException(413, "Document exceeds page budget") from exc
            raise HTTPException(400, "Document page failed") from exc
        finally:
            try:
                if stream is not None:
                    stream.close()
            finally:
                cursor_budget.release(app.app_id)
        return {"rows": rows, "nextSkip": skip + len(rows) if len(rows) == limit else None}

    @router.get("/collections/{collection}/documents/stream")
    async def document_stream(
        collection: str, limit: int | None = Query(None, ge=1),
        page_size: int = Query(128, ge=1, le=1000),
        app: AppTokenClaims = Depends(require_app_token),
    ):
        def open_stream():
            engine = authorize_cursor(app, collection)
            return open_document_stream(engine, collection, page_size=page_size, max_page_bytes=1024 * 1024)
        return await stream_cursor(open_stream, lambda: authorize_cursor(app, collection),
                                   app.app_id, cursor_budget, batch_size=page_size, limit=limit)

    @router.get("/health")
    def health() -> dict[str, str]:
        try:
            if not engine_provider().is_healthy():
                raise RuntimeError("Document engine unhealthy")
            graph_manager_provider()  # Recovery failures must not become HTTP 200.
        except Exception as exc:
            raise HTTPException(status_code=503, detail={"message": "Database is not ready"}) from exc
        return {"message": "ok"}

    @router.post("/query", response_model=QueryExecuteResponse)
    def execute_query_route(
        payload: QueryExecuteRequest,
        request: Request,
        idempotency_key: str | None = Header(None, alias="Idempotency-Key", max_length=128),
        app: AppTokenClaims = Depends(require_app_token),
    ) -> QueryExecuteResponse:
        require_scope(app, "query:execute")
        if app.managed and not engine_provider().is_internal_app_token_active(app.token_id):
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail={"message": "application token has been revoked"},
            )
        def execute():
            return execute_query(
                engine=engine_provider(), graph_manager=graph_manager_provider(),
                payload=payload, granted_scopes=frozenset(app.scopes),
                granted_collections=getattr(app, "collections", None),
                controller=getattr(request.app.state, "execution_controller", None),
                owner="app:"+app.app_id,
            )
        if idempotency_key is None:
            return execute()
        # Include current scopes and token identity: a replay cannot cross
        # principals or return a previous response after scope reduction.
        identity = [app.app_id, app.token_id, sorted(app.scopes)]
        grants = getattr(app, "collections", None)
        if grants is not None:
            identity.append(grants)
        principal = json.dumps(identity, sort_keys=True)
        journal = RequestJournal(request.app.state.settings.db_path / "api_requests.sqlite3")
        def execute_durably():
            result = execute()
            # Never durably cache success ahead of buffered native writes.
            # This conservative barrier is intentionally more expensive than
            # ordinary execution; future same-RocksDB transactional receipts
            # can remove the extra independent journal/barrier cost.
            if not engine_provider().flush_storage().success:
                raise HTTPException(503, "Durability barrier failed; outcome unknown")
            manager = graph_manager_provider()
            if manager is not None:
                for name in manager.list_graphs():
                    if not manager.checkpoint_graph(name):
                        raise HTTPException(503, "Graph durability barrier failed; outcome unknown")
            return result.model_dump()
        return journal.run(principal, idempotency_key, payload.model_dump(),
                           execute_durably)

    @router.post("/query/bson", response_model=QueryExecuteResponse,
                 openapi_extra={"requestBody": {"required": True, "content": {
                     "application/bson": {"schema": {"type": "string", "format": "binary"}}}}})
    async def execute_bson_route(request: Request,
                                 idempotency_key: str | None = Header(None, alias="Idempotency-Key", max_length=128),
                                 app: AppTokenClaims = Depends(require_app_token)):
        # Only request encoding changes: reuse all query authorization,
        # graph integration, durable replay and response handling.
        require_scope(app, "query:execute")
        if request.headers.get('content-type', '').split(';', 1)[0].strip().lower() != 'application/bson':
            raise HTTPException(415, "Expected application/bson")
        body = await request.body()
        if len(body)>2*1024*1024:
            raise HTTPException(413, "BSON request exceeds 2MiB")
        if len(body)<5:
            raise HTTPException(400, "Invalid BSON request length")
        def decode_and_execute():
            engine = engine_provider()
            if app.managed and not engine.is_internal_app_token_active(app.token_id):
                raise HTTPException(401, "application token has been revoked")
            native = load_native_module(request.app.state.settings)
            if not hasattr(native, 'decode_bson_json'):
                raise HTTPException(503, "Rebuild native engine for binary requests")
            try:
                payload = QueryExecuteRequest.model_validate_json(native.decode_bson_json(body))
            except ValueError as exc:
                raise HTTPException(400, "Invalid BSON query envelope") from exc
            return execute_query_route(payload, request, idempotency_key, app)
        return await anyio.to_thread.run_sync(decode_and_execute)

    return router


def create_api_app(
    *,
    settings: AdminApiSettings | None = None,
    engine: Any | None = None,
) -> FastAPI:
    api_settings = settings or AdminApiSettings()
    api_settings.validate_for_startup()
    state: dict[str, Any] = {
        "engine": engine,
        "graph_manager": None,
        "native_module": None,
    }

    app = FastAPI(
        title="NexoraDB External API",
        version=__version__,
        docs_url="/docs" if api_settings.environment != "production" else None,
        redoc_url=None,
    )
    app.state.settings = api_settings
    initialization_lock = RLock()
    def release_resources():
        state["graph_manager"] = None
        state["engine"] = None
    lifecycle = install_lifecycle(app, lambda: (state["engine"], state["graph_manager"]), release_resources)
    app.add_middleware(AdmissionMiddleware, max_inflight=api_settings.api_max_inflight,
                       require_https=api_settings.environment.lower() == "production", tracker=lifecycle.requests)

    def get_engine() -> Any:
        with initialization_lock:
            if state["engine"] is None:
                state["engine"] = create_doc_engine(api_settings)
            return state["engine"]

    def get_native_module() -> Any:
        with initialization_lock:
            if state["native_module"] is None:
                state["native_module"] = load_native_module(api_settings)
            return state["native_module"]

    def get_graph_manager() -> Any | None:
        with initialization_lock:
            native = get_native_module()
            if not getattr(native, "GRAPH_ENABLED", False):
                return None
            if state["graph_manager"] is None:
                state["graph_manager"] = create_graph_manager(api_settings, get_engine())
            return state["graph_manager"]

    app.include_router(
        create_api_router(
            engine_provider=get_engine,
            graph_manager_provider=get_graph_manager,
        )
    )
    return app


app = create_api_app()
