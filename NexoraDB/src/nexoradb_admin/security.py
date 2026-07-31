from __future__ import annotations

import base64
import hashlib
import hmac
import json
import time
from typing import Any

from argon2 import PasswordHasher
from argon2.exceptions import Argon2Error, VerifyMismatchError

from .config import AdminApiSettings

_PASSWORD_HASHER = PasswordHasher(time_cost=3, memory_cost=65536, parallelism=4)


class TokenError(ValueError):
    """Raised when a bearer token is missing, expired, or invalid."""


def now_ms() -> int:
    return int(time.time() * 1000)


def hash_password(password: str) -> str:
    return _PASSWORD_HASHER.hash(password)


def verify_password(password: str, password_hash: str) -> bool:
    try:
        return _PASSWORD_HASHER.verify(password_hash, password)
    except (Argon2Error, VerifyMismatchError):
        return False


def validate_password_strength(password: str) -> None:
    if len(password) < 12:
        raise ValueError("password must be at least 12 characters")
    if not any(ch.islower() for ch in password):
        raise ValueError("password must contain a lowercase letter")
    if not any(ch.isupper() for ch in password):
        raise ValueError("password must contain an uppercase letter")
    if not any(ch.isdigit() for ch in password):
        raise ValueError("password must contain a number")


def _b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64url_decode(data: str) -> bytes:
    padding = "=" * (-len(data) % 4)
    return base64.urlsafe_b64decode((data + padding).encode("ascii"))


def _json_b64(data: dict[str, Any]) -> str:
    raw = json.dumps(data, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return _b64url_encode(raw)


def create_access_token(
    *,
    username: str,
    role: str,
    settings: AdminApiSettings,
) -> str:
    now = int(time.time())
    header = {"alg": "HS256", "typ": "JWT"}
    payload = {
        "sub": username,
        "role": role,
        "iat": now,
        "exp": now + settings.access_token_ttl_seconds,
        "typ": "access",
    }
    signing_input = f"{_json_b64(header)}.{_json_b64(payload)}"
    signature = hmac.new(
        settings.auth_secret.encode("utf-8"),
        signing_input.encode("ascii"),
        hashlib.sha256,
    ).digest()
    return f"{signing_input}.{_b64url_encode(signature)}"


def decode_access_token(token: str, settings: AdminApiSettings) -> dict[str, Any]:
    try:
        header_raw, payload_raw, signature_raw = token.split(".", 2)
        signing_input = f"{header_raw}.{payload_raw}"
        expected_signature = hmac.new(
            settings.auth_secret.encode("utf-8"),
            signing_input.encode("ascii"),
            hashlib.sha256,
        ).digest()
        actual_signature = _b64url_decode(signature_raw)
        if not hmac.compare_digest(expected_signature, actual_signature):
            raise TokenError("invalid token signature")

        header = json.loads(_b64url_decode(header_raw))
        payload = json.loads(_b64url_decode(payload_raw))
    except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise TokenError("invalid token") from exc

    if header.get("alg") != "HS256" or payload.get("typ") != "access":
        raise TokenError("invalid token")
    if int(payload.get("exp", 0)) < int(time.time()):
        raise TokenError("token expired")
    if not payload.get("sub"):
        raise TokenError("invalid token subject")
    return payload
