from __future__ import annotations

import json
from typing import Any, Protocol

from fastapi import HTTPException, status

from .config import AdminApiSettings
from .models import AdminRegisterRequest, AuthResponse, PublicUser
from .security import (
    create_access_token,
    hash_password,
    now_ms,
    validate_password_strength,
    verify_password,
)


class DBResultLike(Protocol):
    success: bool
    data: str
    error_msg: str


class InternalUserEngine(Protocol):
    def create_internal_user(self, user_json: str) -> DBResultLike: ...

    def get_internal_user(self, username: str) -> DBResultLike: ...

    def update_internal_user(self, username: str, user_json: str) -> DBResultLike: ...


def _db_error_to_http(error_msg: str) -> HTTPException:
    if error_msg == "internal user not found":
        return HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail={"message": error_msg})
    if error_msg == "internal user already exists":
        return HTTPException(status_code=status.HTTP_409_CONFLICT, detail={"message": error_msg})
    return HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail={"message": error_msg})


def _load_user(engine: InternalUserEngine, username: str) -> dict[str, Any]:
    result = engine.get_internal_user(username)
    if not result.success:
        raise _db_error_to_http(result.error_msg)
    try:
        user = json.loads(result.data)
    except json.JSONDecodeError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail={"message": "stored internal user document is invalid"},
        ) from exc
    if not isinstance(user, dict):
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail={"message": "stored internal user document is invalid"},
        )
    return user


def _public_user(user: dict[str, Any]) -> PublicUser:
    return PublicUser.model_validate(
        {
            "_id": user.get("_id", ""),
            "username": user.get("username", ""),
            "email": user.get("email"),
            "role": user.get("role", "application"),
            "firstName": user.get("first_name"),
            "lastName": user.get("last_name"),
            "status": user.get("status", "disabled"),
            "createdAt": user.get("created_at", 0),
            "updatedAt": user.get("updated_at", 0),
            "lastLoginAt": user.get("last_login_at"),
        }
    )


def root_exists(engine: InternalUserEngine) -> bool:
    result = engine.get_internal_user("root")
    if result.success:
        return True
    if result.error_msg == "internal user not found":
        return False
    raise _db_error_to_http(result.error_msg)


def register_root_admin(
    engine: InternalUserEngine,
    payload: AdminRegisterRequest,
) -> PublicUser:
    if root_exists(engine):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail={"message": "root user already exists"},
        )

    try:
        validate_password_strength(payload.password)
    except ValueError as exc:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail={"message": str(exc)},
        ) from exc

    timestamp = now_ms()
    user = {
        "_id": "usr_root",
        "username": "root",
        "email": payload.email,
        "password_hash": hash_password(payload.password),
        "role": "admin",
        "first_name": payload.first_name,
        "last_name": payload.last_name,
        "status": "active",
        "created_at": timestamp,
        "updated_at": timestamp,
        "last_login_at": None,
    }
    result = engine.create_internal_user(json.dumps(user, separators=(",", ":")))
    if not result.success:
        raise _db_error_to_http(result.error_msg)
    return _public_user(user)


def login_admin(
    engine: InternalUserEngine,
    username: str,
    password: str,
    settings: AdminApiSettings,
) -> AuthResponse:
    try:
        user = _load_user(engine, username)
    except HTTPException as exc:
        if exc.status_code == status.HTTP_404_NOT_FOUND:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail={"message": "invalid username or password"},
            ) from exc
        raise

    if user.get("status") != "active" or user.get("role") != "admin":
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"message": "admin account is not active"},
        )
    if not verify_password(password, str(user.get("password_hash", ""))):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail={"message": "invalid username or password"},
        )

    user["last_login_at"] = now_ms()
    user["updated_at"] = user["last_login_at"]
    update = engine.update_internal_user(username, json.dumps(user, separators=(",", ":")))
    if not update.success:
        raise _db_error_to_http(update.error_msg)

    token = create_access_token(username=username, role=str(user["role"]), settings=settings)
    return AuthResponse.model_validate(
        {
            "accessToken": token,
            "tokenType": "bearer",
            "expiresIn": settings.access_token_ttl_seconds,
            "user": _public_user(user).model_dump(by_alias=True),
        }
    )


def get_current_public_user(engine: InternalUserEngine, username: str) -> PublicUser:
    user = _load_user(engine, username)
    if user.get("status") != "active":
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"message": "account is not active"},
        )
    return _public_user(user)
