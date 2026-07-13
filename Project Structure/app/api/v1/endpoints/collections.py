from typing import Optional
from fastapi import APIRouter, Depends, HTTPException

from app.api.v1.models.schema import SchemaDefinition, IndexDefinition, ForeignKeyDefinition
from app.api.v1.models.response import StandardResponse
from app.api.v1.models.security import AppTokenClaims
from app.core.dependencies import require_app_token, require_scope
from app.services.engine_provider import get_engine

router = APIRouter(prefix="/collections", tags=["Collections"])


@router.post("/", response_model=StandardResponse)
async def create_collection(
    name: str,
    schema: Optional[SchemaDefinition] = None,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Create a new collection. If schema is provided, validation is enabled."""
    require_scope(app, "collections:write")
    engine = get_engine()
    result = engine.CreateCollection(name, schema)
    if not result["success"]:
        raise HTTPException(status_code=400, detail=result["error_msg"])
    return StandardResponse.ok({"message": f"Collection '{name}' created"})


@router.delete("/{collection}", response_model=StandardResponse)
async def drop_collection(
    collection: str,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Drop a collection - Irreversible! All documents, indexes, FK removed."""
    require_scope(app, "collections:write")
    # TODO: Connect to DocEngine::DropCollection
    return StandardResponse.ok({"message": f"Collection '{collection}' dropped"})


@router.get("/", response_model=StandardResponse)
async def list_collections(
    app: AppTokenClaims = Depends(require_app_token),
):
    """List all collections."""
    require_scope(app, "collections:read")
    engine = get_engine()
    return StandardResponse.ok(engine.ListCollections())


@router.get("/{collection}/exists", response_model=StandardResponse)
async def collection_exists(
    collection: str,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Check if a collection exists - O(1)."""
    require_scope(app, "collections:read")
    # TODO: Connect to DocEngine::CollectionExists
    return StandardResponse.ok({"exists": True})


@router.post("/{collection}/index", response_model=StandardResponse)
async def create_index(
    collection: str,
    index: IndexDefinition,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Create a new index on the collection."""
    require_scope(app, "collections:write")
    # TODO: Connect to DocEngine::CreateIndex
    return StandardResponse.ok({"message": f"Index '{index.index_name}' created"})


@router.post("/{collection}/foreign-key", response_model=StandardResponse)
async def add_foreign_key(
    collection: str,
    fk: ForeignKeyDefinition,
    app: AppTokenClaims = Depends(require_app_token),
):
    """Add a foreign key constraint."""
    require_scope(app, "collections:write")
    # TODO: Connect to DocEngine::AddForeignKey
    return StandardResponse.ok({"message": f"Foreign key '{fk.fk_name}' added"})