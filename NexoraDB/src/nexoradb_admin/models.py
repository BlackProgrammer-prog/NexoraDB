from __future__ import annotations

import re
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")


class AdminRegisterRequest(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    first_name: str = Field(alias="firstName", min_length=1, max_length=100)
    last_name: str = Field(alias="lastName", min_length=1, max_length=100)
    email: str = Field(min_length=3, max_length=254)
    password: str = Field(min_length=1, max_length=1024)
    confirm_password: str | None = Field(default=None, alias="confirmPassword")

    @field_validator("first_name", "last_name", "email", "password", "confirm_password")
    @classmethod
    def strip_text(cls, value: str | None) -> str | None:
        return value.strip() if isinstance(value, str) else value

    @field_validator("email")
    @classmethod
    def validate_email(cls, value: str) -> str:
        if not EMAIL_RE.match(value):
            raise ValueError("email is invalid")
        return value.lower()

    @model_validator(mode="after")
    def validate_password_match(self) -> "AdminRegisterRequest":
        if self.confirm_password is not None and self.password != self.confirm_password:
            raise ValueError("password and confirmPassword do not match")
        return self


class LoginRequest(BaseModel):
    username: str = Field(min_length=1, max_length=100)
    password: str = Field(min_length=1, max_length=1024)

    @field_validator("username", "password")
    @classmethod
    def strip_text(cls, value: str) -> str:
        return value.strip()


class PublicUser(BaseModel):
    id: str = Field(alias="_id")
    username: str
    email: str | None = None
    role: Literal["admin", "application"]
    first_name: str | None = Field(default=None, alias="firstName")
    last_name: str | None = Field(default=None, alias="lastName")
    status: Literal["active", "disabled", "deleted"]
    created_at: int = Field(alias="createdAt")
    updated_at: int = Field(alias="updatedAt")
    last_login_at: int | None = Field(default=None, alias="lastLoginAt")
    display_name: str = Field(alias="displayName")


class AuthResponse(BaseModel):
    access_token: str = Field(alias="accessToken")
    token_type: Literal["bearer"] = Field(default="bearer", alias="tokenType")
    expires_in: int = Field(alias="expiresIn")
    user: PublicUser


class SetupStateResponse(BaseModel):
    needs_setup: bool = Field(alias="needsSetup")


class MessageResponse(BaseModel):
    message: str
