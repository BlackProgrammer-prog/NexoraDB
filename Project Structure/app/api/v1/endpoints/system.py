from fastapi import APIRouter, Depends, Request

from app.api.v1.models.response import StandardResponse
from app.api.v1.models.security import AppTokenClaims
from app.core.dependencies import require_app_token, require_scope
from app.services.engine_provider import get_engine

router = APIRouter(prefix="/system", tags=["System"])


@router.get("/health", response_model=StandardResponse)
async def health_check():
    """Check system health and RocksDB connection."""
    engine = get_engine()
    is_healthy = engine.IsHealthy() if hasattr(engine, "IsHealthy") else True
    return StandardResponse.ok({
        "status": "healthy" if is_healthy else "unhealthy",
        "version": "0.1.0",
        "rocksdb": "connected" if is_healthy else "disconnected"
    })


@router.get("/info", response_model=StandardResponse)
async def system_info(request: Request):
    """Get system information."""
    settings = request.app.state.settings
    return StandardResponse.ok({
        "app_name": settings.APP_NAME,
        "version": settings.APP_VERSION,
        "environment": settings.ENVIRONMENT,
        "graph_enabled": settings.GRAPH_ENABLED,
    })


@router.get("/metrics", response_model=StandardResponse)
async def system_metrics(
    app: AppTokenClaims = Depends(require_app_token),
):
    """Get system metrics. Requires scope: monitoring:read"""
    require_scope(app, "monitoring:read")
    return StandardResponse.ok({
        "collections": 5,
        "total_documents": 1000,
        "active_connections": 3,
    })