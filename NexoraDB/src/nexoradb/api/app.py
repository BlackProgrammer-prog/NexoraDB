"""
nexoradb/api/app.py
External API router — mounted by nexoradb_admin into the main FastAPI app.
Provides the /api/v1/* endpoints used by external applications (app tokens).
"""
from __future__ import annotations

from collections.abc import Callable
from typing import Any

from fastapi import APIRouter, Depends, Header, HTTPException, Request, status

from .security import AVAILABLE_APP_SCOPES


# ── Token validation ─────────────────────────────────────────────────────────

def _decode_app_token(token: str) -> dict[str, Any]:
    """Decode an app JWT without verifying signature (verification happens in admin layer)."""
    import base64
    import json

    try:
        parts = token.split(".")
        if len(parts) != 3:
            raise ValueError("invalid token structure")
        padding = "=" * (-len(parts[1]) % 4)
        payload = json.loads(
            base64.urlsafe_b64decode((parts[1] + padding).encode()).decode()
        )
        return payload
    except Exception as exc:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail={"message": f"invalid app token: {exc}"},
        ) from exc


def _require_scope(required: str) -> Callable[..., dict[str, Any]]:
    def dependency(authorization: str | None = Header(default=None)) -> dict[str, Any]:
        if not authorization or not authorization.startswith("Bearer "):
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail={"message": "missing bearer token"},
            )
        payload = _decode_app_token(authorization.removeprefix("Bearer ").strip())
        scopes: list[str] = payload.get("scopes", [])
        if required not in scopes:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail={"message": f"scope '{required}' is required"},
            )
        return payload
    return dependency


# ── Router factory ───────────────────────────────────────────────────────────

def create_api_router(
    *,
    engine_provider: Callable[[], Any],
    graph_manager_provider: Callable[[], Any | None],
) -> APIRouter:
    """
    Returns an APIRouter with all /api/v1/* endpoints.
    Called by nexoradb_admin/app.py via api_bridge.
    """
    router = APIRouter(prefix="/api/v1", tags=["external-api"])

    # ── health ────────────────────────────────────────────────────────────────

    @router.get("/health")
    def api_health() -> dict[str, str]:
        return {"status": "ok"}

    # ── collections ───────────────────────────────────────────────────────────

    @router.get("/collections")
    def api_list_collections(
        _: dict[str, Any] = Depends(_require_scope("collections:read")),
        engine: Any = Depends(engine_provider),
    ) -> list[dict[str, Any]]:
        try:
            return engine.list_collections() or []
        except Exception as exc:
            raise HTTPException(status_code=500, detail={"message": str(exc)}) from exc

    @router.get("/collections/{collection_name}/documents")
    def api_list_documents(
        collection_name: str,
        _: dict[str, Any] = Depends(_require_scope("documents:read")),
        engine: Any = Depends(engine_provider),
    ) -> list[dict[str, Any]]:
        try:
            return engine.list_documents(collection_name) or []
        except Exception as exc:
            raise HTTPException(status_code=500, detail={"message": str(exc)}) from exc

    @router.get("/collections/{collection_name}/documents/{doc_id}")
    def api_get_document(
        collection_name: str,
        doc_id: str,
        _: dict[str, Any] = Depends(_require_scope("documents:read")),
        engine: Any = Depends(engine_provider),
    ) -> dict[str, Any]:
        try:
            result = engine.get_document(collection_name, doc_id)
            if result is None:
                raise HTTPException(status_code=404, detail={"message": "document not found"})
            return result
        except HTTPException:
            raise
        except Exception as exc:
            raise HTTPException(status_code=500, detail={"message": str(exc)}) from exc

    # ── graphs ────────────────────────────────────────────────────────────────

    @router.get("/graphs")
    def api_list_graphs(
        _: dict[str, Any] = Depends(_require_scope("graphs:read")),
        graph_manager: Any = Depends(graph_manager_provider),
    ) -> list[dict[str, Any]]:
        if graph_manager is None:
            return []
        try:
            names = graph_manager.list_graphs() or []
            return [{"name": n} for n in names]
        except Exception as exc:
            raise HTTPException(status_code=500, detail={"message": str(exc)}) from exc

    # ── query ─────────────────────────────────────────────────────────────────

    @router.post("/query/execute")
    def api_execute_query(
        payload: dict[str, Any],
        _: dict[str, Any] = Depends(_require_scope("query:execute")),
        engine: Any = Depends(engine_provider),
    ) -> dict[str, Any]:
        from nexoradb_admin.query_runner import QueryExecuteRequest, execute_query
        req = QueryExecuteRequest(query=payload.get("query", ""))
        result = execute_query(
            engine=engine,
            graph_manager=graph_manager_provider(),
            payload=req,
        )
        return result.model_dump()

    # ── scopes ────────────────────────────────────────────────────────────────

    @router.get("/scopes")
    def api_scopes() -> dict[str, list[str]]:
        return {"scopes": list(AVAILABLE_APP_SCOPES)}

    return router
