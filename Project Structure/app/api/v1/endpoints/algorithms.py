from typing import Optional
from fastapi import APIRouter, Depends

from app.api.v1.models.graph import AlgorithmParams, JobStatus
from app.api.v1.models.response import StandardResponse
from app.api.v1.models.security import AppTokenClaims
from app.core.dependencies import require_app_token, require_scope

router = APIRouter(prefix="/algorithms", tags=["Algorithms"])


@router.post("/lock", response_model=StandardResponse)
async def run_lock_algorithm(
    graph_name: str,
    algorithm: AlgorithmParams,
    app: AppTokenClaims = Depends(require_app_token),
):
    """
    Run lightweight algorithm on LiveGraph (< 100ms).
    
    Algorithms: CommonFollowers, SuggestFollowers, ShortestPath, AreConnected
    """
    require_scope(app, "graphs:read")
    # TODO: Connect to LockAlgorithm
    return StandardResponse.ok({
        "algorithm": algorithm.name,
        "result": ["user_001", "user_002"],
        "elapsed_ms": 45.2
    })


@router.post("/job", response_model=StandardResponse)
async def run_job_algorithm(
    graph_name: str,
    algorithm: AlgorithmParams,
    app: AppTokenClaims = Depends(require_app_token),
):
    """
    Run heavy algorithm on StaticGraphView (> 100ms).
    
    Async via Job ID. Algorithms: PageRank, CommunityDetection, NetworkAnalysis
    """
    require_scope(app, "graphs:read")
    job_id = "job_" + str(hash(f"{graph_name}_{algorithm.name}"))[:8]
    return StandardResponse.ok({
        "job_id": job_id,
        "status": JobStatus.PENDING,
        "check_status": f"/api/v1/algorithms/job/{job_id}/status"
    })


@router.get("/job/{job_id}/status", response_model=StandardResponse)
async def get_job_status(
    job_id: str,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Get job execution status."""
    require_scope(app, "graphs:read")
    return StandardResponse.ok({
        "job_id": job_id,
        "status": JobStatus.COMPLETED,
        "progress": 1.0
    })