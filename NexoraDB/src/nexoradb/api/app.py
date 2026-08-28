from __future__ import annotations

from collections.abc import Callable
from typing import Any

from fastapi import APIRouter, Depends, FastAPI, HTTPException, status

from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import create_doc_engine, create_graph_manager, load_native_module
from nexoradb_admin.query_runner import QueryExecuteRequest, QueryExecuteResponse, execute_query

from .security import AppTokenClaims, require_app_token, require_scope


def create_api_router(
    *,
    engine_provider: Callable[[], Any],
    graph_manager_provider: Callable[[], Any | None],
) -> APIRouter:
    router = APIRouter(prefix="/api/v1", tags=["NexoraDB API"])

    @router.get("/health")
    def health() -> dict[str, str]:
        return {"message": "ok"}

    @router.post("/query", response_model=QueryExecuteResponse)
    def execute_query_route(
        payload: QueryExecuteRequest,
        app: AppTokenClaims = Depends(require_app_token),
    ) -> QueryExecuteResponse:
        require_scope(app, "query:execute")
        if app.managed and not engine_provider().is_internal_app_token_active(app.token_id):
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail={"message": "application token has been revoked"},
            )
        return execute_query(
            engine=engine_provider(),
            graph_manager=graph_manager_provider(),
            payload=payload,
        )

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
        version="0.1.1",
        docs_url="/docs" if api_settings.environment != "production" else None,
        redoc_url=None,
    )
    app.state.settings = api_settings

    def get_engine() -> Any:
        if state["engine"] is None:
            state["engine"] = create_doc_engine(api_settings)
        return state["engine"]

    def get_native_module() -> Any:
        if state["native_module"] is None:
            state["native_module"] = load_native_module(api_settings)
        return state["native_module"]

    def get_graph_manager() -> Any | None:
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
