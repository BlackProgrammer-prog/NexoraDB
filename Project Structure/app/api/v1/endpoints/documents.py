from typing import Optional
from fastapi import APIRouter, Depends, HTTPException

from app.api.v1.models.query import Condition, QueryOptions
from app.api.v1.models.update import UpdateSpec
from app.api.v1.models.response import StandardResponse
from app.api.v1.models.security import AppTokenClaims
from app.core.dependencies import require_app_token, require_scope
from app.core.exceptions import DocumentNotFoundError
from app.services.engine_provider import get_engine

router = APIRouter(prefix="/documents", tags=["Documents"])


@router.post("/{collection}", response_model=StandardResponse)
async def insert_one(
    collection: str,
    document: dict,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Insert one document into collection. Auto-generates _id if missing."""
    require_scope(app, "documents:write")
    engine = get_engine()
    result = engine.InsertOne(collection, document)
    if not result["success"]:
        raise HTTPException(status_code=400, detail=result["error_msg"])
    return StandardResponse.ok({"inserted_id": result["data"]})


@router.get("/{collection}/{doc_id}", response_model=StandardResponse)
async def find_by_id(
    collection: str,
    doc_id: str,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Find document by ID - O(1) fastest operation."""
    require_scope(app, "documents:read")
    engine = get_engine()
    result = engine.FindById(collection, doc_id)
    if not result["success"]:
        raise DocumentNotFoundError(doc_id, collection)
    return StandardResponse.ok(result["data"])


@router.post("/{collection}/find", response_model=StandardResponse)
async def find_many(
    collection: str,
    condition: Condition,
    options: QueryOptions,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Find documents with condition. Uses index scan if available."""
    require_scope(app, "documents:read")
    # TODO: Connect to DocEngine::FindMany
    return StandardResponse.ok([
        {"_id": "u1", "username": "alice", "age": 25},
        {"_id": "u2", "username": "bob", "age": 30},
    ])


@router.patch("/{collection}/{doc_id}", response_model=StandardResponse)
async def update_by_id(
    collection: str,
    doc_id: str,
    update_spec: UpdateSpec,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Update document by ID - Atomic Read-Modify-Write."""
    require_scope(app, "documents:write")
    # TODO: Connect to DocEngine::UpdateById
    return StandardResponse.ok({"modified_count": 1})


@router.delete("/{collection}/{doc_id}", response_model=StandardResponse)
async def delete_by_id(
    collection: str,
    doc_id: str,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Delete document by ID."""
    require_scope(app, "documents:write")
    # TODO: Connect to DocEngine::DeleteById
    return StandardResponse.ok({"deleted": True})