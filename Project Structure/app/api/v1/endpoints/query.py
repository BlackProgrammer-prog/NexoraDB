from fastapi import APIRouter, Depends

from app.api.v1.models.security import AppTokenClaims
from app.api.v1.models.query_runner import QueryExecuteRequest, QueryExecuteResponse, execute_query
from app.core.dependencies import require_app_token, require_scope
from app.services.engine_provider import get_engine, get_graph_manager

router = APIRouter(prefix="/query", tags=["Query"])


@router.post("/", response_model=QueryExecuteResponse)
async def execute_query_endpoint(
    payload: QueryExecuteRequest,
    app: AppTokenClaims = Depends(require_app_token),
) -> QueryExecuteResponse:
    """
    Execute a NexoraQL query.
    
    Requires scope: query:execute
    
    Examples:
        - SELECT * FROM users WHERE age > 18 LIMIT 10;
        - INSERT INTO users VALUES ('{"username":"alice"}');
        - CREATE COLLECTION posts;
        - SHOW COLLECTIONS;
    """
    require_scope(app, "query:execute")
    
    return execute_query(
        engine=get_engine(),
        graph_manager=get_graph_manager(),
        payload=payload,
    )