"""
nexoradb/api/security.py
App token creation and scope definitions for external application access.
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import time
from dataclasses import dataclass
from typing import Any

from pydantic import BaseModel, Field

# ── Available scopes ────────────────────────────────────────────────────────

AVAILABLE_APP_SCOPES: list[str] = [
    "query:execute",
    "collections:read",
    "collections:write",
    "documents:read",
    "documents:write",
    "graphs:read",
    "graphs:write",
    "monitoring:read",
    "admin:apps",
]


# ── Pydantic models ──────────────────────────────────────────────────────────

class CreateAppTokenRequest(BaseModel):
    appId: str = Field(min_length=1, max_length=128)
    appName: str | None = None
    expiresInSeconds: int | None = None
    scopes: list[str] = Field(default_factory=list)


class CreateAppTokenResponse(BaseModel):
    token: str
    appId: str
    appName: str | None
    scopes: list[str]
    expiresAt: int | None


# ── Token helpers ────────────────────────────────────────────────────────────

def _b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64url_decode(data: str) -> bytes:
    padding = "=" * (-len(data) % 4)
    return base64.urlsafe_b64decode((data + padding).encode("ascii"))


def _json_b64(data: dict[str, Any]) -> str:
    raw = json.dumps(data, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return _b64url_encode(raw)


def create_app_token(
    *,
    app_id: str,
    app_name: str | None,
    scopes: list[str],
    secret: str,
    expires_in_seconds: int | None = None,
) -> CreateAppTokenResponse:
    now = int(time.time())
    expires_at = (now + expires_in_seconds) if expires_in_seconds else None

    header = {"alg": "HS256", "typ": "JWT"}
    payload: dict[str, Any] = {
        "sub": app_id,
        "app_name": app_name,
        "scopes": scopes,
        "iat": now,
        "typ": "app",
    }
    if expires_at is not None:
        payload["exp"] = expires_at

    signing_input = f"{_json_b64(header)}.{_json_b64(payload)}"
    signature = hmac.new(
        secret.encode("utf-8"),
        signing_input.encode("ascii"),
        hashlib.sha256,
    ).digest()
    token = f"{signing_input}.{_b64url_encode(signature)}"

    return CreateAppTokenResponse(
        token=token,
        appId=app_id,
        appName=app_name,
        scopes=scopes,
        expiresAt=expires_at,
    )
