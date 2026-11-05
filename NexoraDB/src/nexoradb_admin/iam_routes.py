"""Credential endpoints use dedicated bodies, never query text or history."""

from typing import Literal

from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import BaseModel, ConfigDict, Field


class StrictRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")


class CreateIdentity(StrictRequest):
    username: str = Field(min_length=1, max_length=64)
    password: str = Field(min_length=15, max_length=1024, repr=False)
    roles: list[str] = Field(default_factory=list, max_length=16)
    grants: dict[str, list[Literal["read", "write", "manage"]]] = Field(
        default_factory=dict, max_length=128
    )


class UserUpdate(StrictRequest):
    username: str | None = Field(default=None, min_length=1, max_length=64)
    status: Literal["active", "disabled"] | None = None
    roles: list[str] | None = Field(default=None, max_length=16)
    grants: dict[str, list[Literal["read", "write", "manage"]]] | None = Field(
        default=None, max_length=128
    )


class RoleUpdate(StrictRequest):
    parents: list[str] = Field(default_factory=list, max_length=16)
    grants: dict[str, list[Literal["read", "write", "manage"]]] = Field(
        default_factory=dict, max_length=128
    )


class PasswordChange(StrictRequest):
    currentPassword: str = Field(min_length=1, max_length=1024, repr=False)
    newPassword: str = Field(min_length=15, max_length=1024, repr=False)


class PasswordReset(StrictRequest):
    resetToken: str = Field(min_length=72, max_length=72, repr=False)
    newPassword: str = Field(min_length=15, max_length=1024, repr=False)


class RecoveryRotation(StrictRequest):
    currentPassword: str = Field(min_length=1, max_length=1024, repr=False)


def install_iam_routes(app, get_engine, require_session):
    session_dependency = Depends(require_session)

    # Validation errors must never reflect input passwords, reset factors,
    # bearer tokens or whole request bodies into responses/telemetry.
    @app.exception_handler(RequestValidationError)
    async def validation_error(_request: Request, error: RequestValidationError):
        return JSONResponse(
            status_code=422,
            content={
                "message": "Invalid request",
                "detail": [
                    {"loc": list(item["loc"]), "type": item["type"], "msg": item["msg"]}
                    for item in error.errors()
                ],
            },
        )

    router = APIRouter(tags=["Native IAM"])

    def native_engine():
        engine = get_engine()
        if not getattr(engine, "is_native_iam", False):
            raise HTTPException(503, "Native IAM engine required")
        return engine

    @router.post("/auth/logout", status_code=204)
    def logout(_=session_dependency):
        engine = native_engine()
        engine.service.invoke("logout", engine.token)

    @router.post("/auth/change-password")
    def change_password(payload: PasswordChange, _=session_dependency):
        engine = native_engine()
        engine.service.invoke(
            "change_password", engine.token, payload.currentPassword, payload.newPassword
        )
        return {
            "message": (
                "Password changed; all existing sessions and delegated credentials were revoked"
            )
        }

    @router.post("/auth/reset-password")
    def complete_reset(payload: PasswordReset):
        native_engine().service.complete_reset(payload.resetToken, payload.newPassword)
        return {"message": "Password reset complete; sign in with the new password"}

    @router.get("/security/users")
    def users(_=session_dependency):
        return native_engine().administer("list_users")

    @router.post("/security/users", status_code=201)
    def create_user(payload: CreateIdentity, _=session_dependency):
        return native_engine().administer("create_user", payload.model_dump())

    @router.patch("/security/users/{identity_id}")
    def update_user(identity_id: str, payload: UserUpdate, _=session_dependency):
        return native_engine().administer(
            "set_user", {"id": identity_id, **payload.model_dump(exclude_none=True)}
        )

    @router.post("/security/users/{identity_id}/reset")
    def reset_user(identity_id: str, _=session_dependency):
        return native_engine().administer("issue_reset", {"id": identity_id})

    @router.get("/security/roles")
    def roles(_=session_dependency):
        return native_engine().administer("list_roles")

    @router.put("/security/roles/{name}")
    def update_role(name: str, payload: RoleUpdate, _=session_dependency):
        return native_engine().administer("set_role", {"name": name, **payload.model_dump()})

    @router.get("/security/audit")
    def audit(_=session_dependency):
        return native_engine().administer("audit")

    @router.post("/security/recovery/rotate")
    def rotate_recovery(payload: RecoveryRotation, _=session_dependency):
        return native_engine().administer("rotate_recovery", payload.model_dump())

    app.include_router(router)
