from fastapi import APIRouter, Depends, Request

from app.api.v1.models.security import (
    AppTokenClaims,
    CreateAppTokenRequest,
    CreateAppTokenResponse,
    create_app_token,
)
from app.core.dependencies import require_app_token, require_scope

router = APIRouter(prefix="/apps", tags=["Applications"])


@router.post("/tokens", response_model=CreateAppTokenResponse)
async def create_app_token_endpoint(
    request: Request,
    payload: CreateAppTokenRequest,
    admin: AppTokenClaims = Depends(require_app_token),
) -> CreateAppTokenResponse:
    """Create a new application token. Requires scope: admin:apps"""
    require_scope(admin, "admin:apps")
    
    return create_app_token(
        app_id=payload.appId,
        app_name=payload.appName,
        scopes=payload.scopes,
        secret=request.app.state.settings.API_TOKEN_SECRET,
        expires_in_seconds=payload.expiresInSeconds,
    )


@router.get("/tokens", response_model=list[str])
async def list_app_tokens(
    admin: AppTokenClaims = Depends(require_app_token),
) -> list[str]:
    """List all registered application IDs. Requires scope: admin:apps"""
    require_scope(admin, "admin:apps")
    # TODO: Implement token listing from storage
    return ["billing-service", "analytics-service"]