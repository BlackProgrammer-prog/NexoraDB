from typing import Optional
from fastapi import APIRouter, Depends

from app.api.v1.models.graph import Direction, GraphStats
from app.api.v1.models.response import StandardResponse
from app.api.v1.models.security import AppTokenClaims
from app.core.dependencies import require_app_token, require_scope
from app.services.engine_provider import get_graph_manager

router = APIRouter(prefix="/graph", tags=["Graph"])


@router.post("/live", response_model=StandardResponse)
async def create_live_graph(
    name: str,
    condition: Optional[dict] = None,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Create a LiveGraph - always in RAM."""
    require_scope(app, "graphs:write")
    # TODO: Connect to GraphEngine::CreateLiveGraph
    return StandardResponse.ok({"message": f"Live graph '{name}' created"})


@router.post("/{name}/node", response_model=StandardResponse)
async def add_node(
    name: str,
    ext_id: str,
    type_name: str,
    is_implicit: bool = False,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Add a node to the graph. WAL is logged."""
    require_scope(app, "graphs:write")
    graph = get_graph_manager()
    dense_id = graph.addNode(ext_id, type_name, is_implicit)
    return StandardResponse.ok({"dense_id": dense_id})


@router.get("/{name}/node/{ext_id}/neighbors", response_model=StandardResponse)
async def get_neighbors(
    name: str,
    ext_id: str,
    direction: Direction = Direction.OUT,
    edge_type: str = "",
    limit: int = 100,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Get neighbors of a node - O(limit)."""
    require_scope(app, "graphs:read")
    graph = get_graph_manager()
    neighbors = graph.neighborsExt(ext_id, direction.value, edge_type, limit)
    return StandardResponse.ok({"neighbors": neighbors, "count": len(neighbors)})


@router.get("/{name}/stats", response_model=StandardResponse)
async def get_graph_stats(
    name: str,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Get graph statistics - O(1)."""
    require_scope(app, "graphs:read")
    graph = get_graph_manager()
    return StandardResponse.ok(graph.stats())