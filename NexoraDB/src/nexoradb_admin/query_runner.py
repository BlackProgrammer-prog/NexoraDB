from __future__ import annotations

import importlib.util
import json
import sys
import time
from functools import lru_cache
from pathlib import Path
from types import ModuleType
from typing import Any, NoReturn

from fastapi import HTTPException, status
from pydantic import BaseModel, Field

_PARSER_PACKAGE_ALIAS = "_nexoradb_admin_nexoraql"
_MAX_QUERY_LENGTH = 200_000


class QueryExecuteRequest(BaseModel):
    query: str = Field(min_length=1, max_length=_MAX_QUERY_LENGTH)


class QueryExecuteResponse(BaseModel):
    columns: list[str]
    rows: list[dict[str, Any]]
    raw: Any
    executionTimeMs: int
    statementCount: int


@lru_cache(maxsize=1)
def _load_nexoraql_package() -> ModuleType:
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
) -> QueryExecuteResponse:
    query = _normalize_query(payload.query)
    nexoraql = _load_nexoraql_package()
    executor = nexoraql.Executor(engine, graph_manager)

    started_at = time.perf_counter()
    try:
        statement_results = executor.execute_text(query)
    except Exception as exc:
        _raise_query_error(exc, nexoraql)
    execution_time_ms = max(0, round((time.perf_counter() - started_at) * 1000))

    rows = _rows_from_results(statement_results)
    return QueryExecuteResponse(
        columns=_columns_from_rows(rows),
        rows=rows,
        raw={"statements": statement_results},
        executionTimeMs=execution_time_ms,
        statementCount=len(statement_results),
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
