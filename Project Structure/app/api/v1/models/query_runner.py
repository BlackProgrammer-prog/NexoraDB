from __future__ import annotations

from typing import Any, Optional, List
from pydantic import BaseModel, Field


class QueryExecuteRequest(BaseModel):
    query: str = Field(..., min_length=1, description="NexoraQL query string")
    
    class Config:
        json_schema_extra = {"example": {"query": "SELECT * FROM users LIMIT 10;"}}


class QueryExecuteResponse(BaseModel):
    columns: List[str] = Field(default_factory=list)
    rows: List[dict[str, Any]] = Field(default_factory=list)
    raw: Optional[Any] = None
    executionTimeMs: int = 0
    success: bool = True
    error: Optional[str] = None


def execute_query(engine: Any, graph_manager: Any | None, payload: QueryExecuteRequest) -> QueryExecuteResponse:
    import time
    start_time = time.time()
    
    try:
        # TODO: Connect to real C++ parser and engine
        result = _mock_execute(payload.query)
        return QueryExecuteResponse(
            columns=result.get("columns", []),
            rows=result.get("rows", []),
            raw=result.get("raw"),
            executionTimeMs=int((time.time() - start_time) * 1000),
            success=True
        )
    except Exception as e:
        return QueryExecuteResponse(
            success=False,
            error=str(e),
            executionTimeMs=int((time.time() - start_time) * 1000)
        )


def _mock_execute(query: str) -> dict:
    """Mock query execution for development."""
    q = query.lower().strip()
    
    if q.startswith("create collection"):
        return {"columns": ["message"], "rows": [{"message": "Collection created"}]}
    elif q.startswith("show collections"):
        return {"columns": ["name"], "rows": [{"name": "users"}, {"name": "posts"}]}
    elif q.startswith("insert into"):
        return {"columns": ["inserted_id"], "rows": [{"inserted_id": "mock-123"}]}
    elif q.startswith("select"):
        return {
            "columns": ["_id", "username", "age"],
            "rows": [
                {"_id": "u1", "username": "alice", "age": 25},
                {"_id": "u2", "username": "bob", "age": 30},
            ]
        }
    elif q.startswith("count"):
        return {"columns": ["count"], "rows": [{"count": 42}]}
    else:
        return {"columns": ["result"], "rows": [{"result": "Query executed"}]}