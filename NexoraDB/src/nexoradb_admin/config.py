from __future__ import annotations

import os
import secrets
from dataclasses import dataclass, field
from pathlib import Path


def _split_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


@dataclass(frozen=True)
class AdminApiSettings:
    db_path: Path = field(
        default_factory=lambda: Path(os.getenv("NEXORADB_DB_PATH", "./nexoradb-data"))
    )
    auth_secret: str = field(
        default_factory=lambda: os.getenv("NEXORADB_AUTH_SECRET", secrets.token_urlsafe(48))
    )
    access_token_ttl_seconds: int = field(
        default_factory=lambda: int(os.getenv("NEXORADB_ACCESS_TOKEN_TTL_SECONDS", "3600"))
    )
    allowed_origins: tuple[str, ...] = field(
        default_factory=lambda: tuple(
            _split_csv(
                os.getenv(
                    "NEXORADB_ADMIN_ORIGINS",
                    "http://localhost:5173,http://127.0.0.1:5173",
                )
            )
        )
    )
    environment: str = field(default_factory=lambda: os.getenv("NEXORADB_ENV", "development"))
    native_module_path: Path | None = field(
        default_factory=lambda: (
            Path(path) if (path := os.getenv("NEXORADB_NATIVE_MODULE_PATH")) else None
        )
    )

    def validate_for_startup(self) -> None:
        if self.environment.lower() == "production" and "NEXORADB_AUTH_SECRET" not in os.environ:
            raise RuntimeError("NEXORADB_AUTH_SECRET is required in production")
        if len(self.auth_secret) < 32:
            raise RuntimeError("NEXORADB_AUTH_SECRET must be at least 32 characters")
