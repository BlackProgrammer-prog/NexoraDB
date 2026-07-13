from fastapi import HTTPException, Request, status
from fastapi.security import HTTPBearer

from app.api.v1.models.security import AppTokenClaims, AppTokenError, decode_app_token

_bearer = HTTPBearer(auto_error=False)


async def require_app_token(request: Request) -> AppTokenClaims:
    credentials = await _bearer(request)
    if credentials is None or credentials.scheme.lower() != "bearer":
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail={"message": "missing application bearer token"},
        )

    try:
        claims = decode_app_token(
            credentials.credentials,
            request.app.state.settings.API_TOKEN_SECRET
        )
    except AppTokenError as exc:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail={"message": str(exc)},
        ) from exc

    request.state.nexoradb_app_id = claims.app_id
    request.state.nexoradb_app_name = claims.app_name
    return claims


def require_scope(claims: AppTokenClaims, scope: str) -> None:
    if scope not in claims.scopes:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"message": f"application token is missing scope '{scope}'"},
        )