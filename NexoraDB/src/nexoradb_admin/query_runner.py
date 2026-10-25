from __future__ import annotations

import importlib.util
import json
import sys
import time
from functools import lru_cache
from pathlib import Path
from types import ModuleType
from typing import Any, NoReturn
from threading import RLock

from fastapi import HTTPException, status
from pydantic import BaseModel, Field

_PARSER_PACKAGE_ALIAS = "_nexoradb_admin_nexoraql"
_MAX_QUERY_LENGTH = 200_000
_MAX_STATEMENTS = 256
_PARSER_LOCK = RLock()

# An application execution scope is not permission to execute arbitrary DDL.
# Unknown statement types are denied until their access requirements are defined.
_STATEMENT_SCOPES = {
    **dict.fromkeys(("Select", "Count", "ExistsStmt"), ("documents:read",)),
    **dict.fromkeys(("Insert", "InsertBatch", "Update", "Delete"), ("documents:write",)),
    **dict.fromkeys(("ShowCollections", "CollectionExists", "DescribeCollection",
                     "ShowIndexes", "ShowForeignKeys"), ("collections:read",)),
    **dict.fromkeys(("CreateCollection", "DropCollection", "AlterCollection",
                     "CreateIndex", "DropIndex", "AddForeignKey", "DropForeignKey"),
                   ("collections:write",)),
    **dict.fromkeys(("UseGraph", "ShowGraphs", "DescribeGraph", "GraphStatus",
                     "GraphStats", "Traverse", "GetNode", "EdgeExists", "RunLock"),
                   ("graphs:read",)),
    **dict.fromkeys(("CreateGraph", "DropGraph", "BuildGraph", "RenderGraph",
                     "RefreshGraph"), ("graphs:write", "documents:read")),
    **dict.fromkeys(("MapNode", "MapEdge"), ("graphs:write", "documents:read")),
    **dict.fromkeys(("BeginTx", "CommitTx", "RollbackTx"), ()),
    **dict.fromkeys(("SystemStatus", "SystemInfo"), ("monitoring:read",)),
}


def _authorize_statements(statements: list, granted_scopes: frozenset[str]) -> None:
    for statement in statements:
        name = type(statement).__name__
        required = _STATEMENT_SCOPES.get(name)
        if required is None or not set(required).issubset(granted_scopes):
            raise HTTPException(status_code=403, detail={
                "message": f"Application token is not authorized for {name}"})


def authorize_collection(grants, collection, operation):
    if collection.startswith("__nexora_"):
        raise HTTPException(403, "Reserved collection")
    if grants is not None and operation not in grants.get(collection, []):
        raise HTTPException(403, "Collection access denied")


def authorize_collections(statements, grants):
    if grants is None:
        return  # Legacy broad tokens; production rejects these at auth.
    read = {"Select", "Count", "ExistsStmt", "ShowIndexes", "ShowForeignKeys"}
    write = {"Insert", "InsertBatch", "Update", "Delete"}
    manage = {"CreateCollection", "DropCollection", "AlterCollection", "CreateIndex", "DropIndex", "AddForeignKey", "DropForeignKey"}
    named_read = {"CollectionExists", "DescribeCollection"}
    for statement in statements:
        name = type(statement).__name__
        if name in {"BeginTx", "CommitTx", "RollbackTx"}:
            continue
        if name not in read | write | manage | named_read:
            # Graph/metadata/global queries can indirectly expose other data.
            raise HTTPException(403, "Operation unavailable to collection-restricted tokens")
        collection = getattr(statement, "collection", None) or getattr(statement, "name", "")
        operation = "read" if name in read | named_read else "write" if name in write else "manage"
        authorize_collection(grants, collection, operation)
        for target, _, _ in getattr(statement, "joins", []):
            authorize_collection(grants, target, "read")
        if name == "AddForeignKey":
            authorize_collection(grants, statement.ref_collection, "read")


def _validate_transaction_blocks(statements: list) -> None:
    active = False
    for statement in statements:
        name = type(statement).__name__
        if name == "BeginTx":
            if active:
                raise HTTPException(status_code=400, detail="Nested transactions are not supported")
            active = True
        elif name in {"CommitTx", "RollbackTx"}:
            if not active:
                raise HTTPException(status_code=400, detail="No active transaction")
            active = False
    if active:
        raise HTTPException(status_code=400, detail="Transactions must finish in the same HTTP request")


class QueryExecuteRequest(BaseModel):
    query: str = Field(min_length=1, max_length=_MAX_QUERY_LENGTH)
    parameters: dict[str, Any] = Field(default_factory=dict)
    include_raw: bool = True


class QueryExecuteResponse(BaseModel):
    columns: list[str]
    rows: list[dict[str, Any]]
    raw: Any
    executionTimeMs: int
    statementCount: int
    statementErrors: list[str] = Field(default_factory=list)


@lru_cache(maxsize=1)
def _load_nexoraql_package() -> ModuleType:
    with _PARSER_LOCK:
        return _load_nexoraql_package_locked()


def _load_nexoraql_package_locked() -> ModuleType:
    """Load the parser under an alias so it does not occupy the native nexoradb module name."""
    if _PARSER_PACKAGE_ALIAS in sys.modules:
        return sys.modules[_PARSER_PACKAGE_ALIAS]

    parser_dir = Path(__file__).resolve().parent.parent / "nexoradb" / "parser"
    init_file = parser_dir / "__init__.py"
    spec = importlib.util.spec_from_file_location(
        _PARSER_PACKAGE_ALIAS,
        init_file,
        submodule_search_locations=[str(parser_dir)],
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load NexoraQL parser from {init_file}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[_PARSER_PACKAGE_ALIAS] = module
    spec.loader.exec_module(module)
    return module


def execute_query(
    *,
    engine: Any,
    graph_manager: Any | None,
    payload: QueryExecuteRequest,
    granted_scopes: frozenset[str] | None = None,
    granted_collections: dict[str, list[str]] | None = None,
    allow_performance_controls: bool = True,
) -> QueryExecuteResponse:
    query = _normalize_query(payload.query)
    nexoraql = _load_nexoraql_package()
    executor = nexoraql.Executor(engine, graph_manager)

    started_at = time.perf_counter()
    try:
        statements = nexoraql.parse(query, payload.parameters)
        if not allow_performance_controls and any(type(s).__name__ in
                {'ShowPerformance', 'SetPerformance', 'ShowIndexAdvice'} for s in statements):
            raise HTTPException(403, 'Performance controls require an administrator')
        if len(statements) > _MAX_STATEMENTS:
            raise HTTPException(status_code=400, detail="Too many query statements")
        _validate_transaction_blocks(statements)
        if granted_scopes is not None:
            _authorize_statements(statements, granted_scopes)
        authorize_collections(statements, granted_collections)
        # Preserve query semantics: LIMIT is a total-result limit, not a batch
        # size. The buffered response byte guard below must never silently
        # rewrite the user's SELECT or report a truncated result as complete.
        statement_results = executor.execute_statements(statements)
    except Exception as exc:
        _raise_query_error(exc, nexoraql)
    finally:
        executor.abort_transaction()
    execution_time_ms = max(0, round((time.perf_counter() - started_at) * 1000))

    rows = _rows_from_results(statement_results)
    if len(json.dumps(statement_results, ensure_ascii=False).encode("utf-8")) > 16 * 1024 * 1024:
        raise HTTPException(status_code=413, detail="Query response exceeds 16MiB; use document streaming")
    return QueryExecuteResponse(
        columns=_columns_from_rows(rows),
        rows=rows,
        raw={"statements": statement_results} if payload.include_raw else None,
        executionTimeMs=execution_time_ms,
        statementCount=len(statement_results),
        statementErrors=[str(result.get("error") or "Query statement failed")
                         for result in statement_results if result.get("success") is False],
    )


def _normalize_query(query: str) -> str:
    normalized = query.strip()
    if not normalized:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail={"message": "Query cannot be empty"},
        )
    return normalized if normalized.endswith(";") else f"{normalized};"


def _raise_query_error(exc: Exception, nexoraql: ModuleType) -> NoReturn:
    nexoraql_error = getattr(nexoraql, "NexoraQLError", None)
    if nexoraql_error is not None and isinstance(exc, nexoraql_error):
        error_payload = exc.to_dict() if hasattr(exc, "to_dict") else {"message": str(exc)}
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail={"message": str(exc), "queryError": error_payload},
        ) from exc

    raise exc


def _rows_from_results(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if len(results) == 1:
        return _rows_from_single_result(results[0])

    return [
        {
            "statement": index,
            "success": result.get("success"),
            "result": _cell_value(result),
        }
        for index, result in enumerate(results, start=1)
    ]


def _rows_from_single_result(result: dict[str, Any]) -> list[dict[str, Any]]:
    algorithm_result = result.get("result")
    if result.get("algo") and isinstance(algorithm_result, dict):
        row = {
            "algorithm": result["algo"],
            "success": result.get("success"),
            "elapsed_ms": result.get("elapsed_ms", 0.0),
            **algorithm_result,
        }
        if result.get("job_id"):
            row["job_id"] = result["job_id"]
        return [_row_from_value(row)]

    for key in ("documents", "collections", "graphs", "foreign_keys", "indexes", "jobs", "nodes"):
        value = result.get(key)
        if isinstance(value, list):
            return [_row_from_named_value(key, item) for item in value]

    if "data" in result and isinstance(result["data"], str):
        parsed = _parse_json_string(result["data"])
        if isinstance(parsed, list):
            return [_row_from_value(item) for item in parsed]
        if isinstance(parsed, dict):
            return [parsed]

    return [_row_from_value(result)] if result else []


def _row_from_value(value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return {str(key): _cell_value(item) for key, item in value.items()}
    return {"value": _cell_value(value)}


def _row_from_named_value(key: str, value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return _row_from_value(value)
    if key in {"collections", "graphs", "indexes", "jobs", "nodes"}:
        return {"name": _cell_value(value)}
    return {"value": _cell_value(value)}


def _cell_value(value: Any) -> Any:
    if isinstance(value, str | int | float | bool) or value is None:
        return value
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _columns_from_rows(rows: list[dict[str, Any]]) -> list[str]:
    columns: list[str] = []
    for row in rows:
        for key in row:
            if key not in columns:
                columns.append(key)
    return columns


def _parse_json_string(value: str) -> Any:
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return value
