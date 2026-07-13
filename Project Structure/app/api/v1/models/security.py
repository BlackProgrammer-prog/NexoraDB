from __future__ import annotations

import base64
import hashlib
import hmac
import json
import secrets
import time
from dataclasses import dataclass
from typing import Any, Optional

from pydantic import BaseModel, Field, field_validator


class AppTokenError(ValueError):
    """Raised when an application token is invalid."""


AVAILABLE_APP_SCOPES: tuple[str, ...] = (
    "query:execute",
    "collections:read",
    "collections:write",
    "documents:read",
    "documents:write",
    "graphs:read",
    "graphs:write",
    "monitoring:read",
    "admin:apps",
)


@dataclass(frozen=True)
class AppTokenClaims:
    app_id: str
    app_name: str
    issued_at: int
    expires_at: int | None
    scopes: tuple[str, ...]


class CreateAppTokenRequest(BaseModel):
    appId: str = Field(min_length=3, max_length=80, pattern=r"^[A-Za-z0-9_.-]+$")
    appName: Optional[str] = Field(default=None, max_length=120)
    scopes: list[str] = Field(default_factory=lambda: ["query:execute"])
    expiresInSeconds: Optional[int] = Field(default=None, ge=300, le=31_536_000)

    @field_validator("scopes")
    @classmethod
    def validate_scopes(cls, value: list[str]) -> list[str]:
        normalized = sorted({scope.strip() for scope in value if scope.strip()})
        invalid = [scope for scope in normalized if scope not in AVAILABLE_APP_SCOPES]
        if invalid:
            raise ValueError(f"invalid app scopes: {', '.join(invalid)}")
        return normalized or ["query:execute"]


class CreateAppTokenResponse(BaseModel):
    appId: str
    appName: str
    token: str
    tokenType: str = "bearer"
    expiresAt: Optional[int] = None
    scopes: list[str]


def create_app_token(
    *,
    app_id: str,
    app_name: str | None,
    scopes: list[str],
    secret: str,
    expires_in_seconds: int | None = None,
) -> CreateAppTokenResponse:
    issued_at = int(time.time())
    expires_at = issued_at + expires_in_seconds if expires_in_seconds is not None else None
    normalized_scopes = sorted({scope.strip() for scope in scopes if scope.strip()})
    if not normalized_scopes:
        normalized_scopes = ["query:execute"]

    header = {"alg": "HS256", "typ": "NexoraDB-App-Token"}
    payload = {
        "app_id": app_id,
        "app_name": app_name or app_id,
        "iat": issued_at,
        "exp": expires_at,
        "jti": secrets.token_urlsafe(16),
        "scopes": normalized_scopes,
        "typ": "app",
    }
    signing_input = f"{_json_b64(header)}.{_json_b64(payload)}"
    signature = hmac.new(
        secret.encode("utf-8"),
        signing_input.encode("ascii"),
        hashlib.sha256,
    ).digest()
    token = f"nxapp_{signing_input}.{_b64url_encode(signature)}"
    return CreateAppTokenResponse(
        appId=app_id,
        appName=payload["app_name"],
        token=token,
        expiresAt=expires_at,
        scopes=normalized_scopes,
    )


def decode_app_token(token: str, secret: str) -> AppTokenClaims:
    if not token.startswith("nxapp_"):
        raise AppTokenError("invalid application token")

    try:
        header_raw, payload_raw, signature_raw = token.removeprefix("nxapp_").split(".", 2)
        signing_input = f"{header_raw}.{payload_raw}"
        expected_signature = hmac.new(
            secret.encode("utf-8"),
            signing_input.encode("ascii"),
            hashlib.sha256,
        ).digest()
        actual_signature = _b64url_decode(signature_raw)
        if not hmac.compare_digest(expected_signature, actual_signature):
            raise AppTokenError("invalid application token signature")

        header = json.loads(_b64url_decode(header_raw))
        payload = json.loads(_b64url_decode(payload_raw))
    except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise AppTokenError("invalid application token") from exc

    if header.get("alg") != "HS256" or payload.get("typ") != "app":
        raise AppTokenError("invalid application token")
    expires_at = payload.get("exp")
    if expires_at is not None and int(expires_at) < int(time.time()):
        raise AppTokenError("application token expired")
    app_id = str(payload.get("app_id") or "")
    if not app_id:
        raise AppTokenError("invalid application token subject")

    scopes = payload.get("scopes")
    return AppTokenClaims(
        app_id=app_id,
        app_name=str(payload.get("app_name") or app_id),
        issued_at=int(payload.get("iat") or 0),
        expires_at=int(expires_at) if expires_at is not None else None,
        scopes=tuple(str(scope) for scope in scopes) if isinstance(scopes, list) else (),
    )


def _b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64url_decode(data: str) -> bytes:
    padding = "=" * (-len(data) % 4)
    return base64.urlsafe_b64decode((data + padding).encode("ascii"))


def _json_b64(data: dict[str, Any]) -> str:
    raw = json.dumps(data, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return _b64url_encode(raw)