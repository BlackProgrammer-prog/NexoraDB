from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

_DEFAULT_DEV_AUTH_SECRET = "nexoradb-admin-development-secret-change-in-production"
_DEFAULT_DEV_API_TOKEN_SECRET = "nexoradb-api-development-token-secret-change-in-production"
_DEFAULT_ACCESS_TOKEN_TTL_SECONDS = 12 * 60 * 60


def _split_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


@dataclass(frozen=True)
class AdminApiSettings:
    execution_query_slots: int = field(default_factory=lambda: int(os.getenv('NEXORADB_EXECUTION_QUERY_SLOTS', '8')))
    execution_per_app: int = field(default_factory=lambda: int(os.getenv('NEXORADB_EXECUTION_PER_APP', '2')))
    execution_queue_size: int = field(default_factory=lambda: int(os.getenv('NEXORADB_EXECUTION_QUEUE_SIZE', '32')))
    execution_wait_ms: int = field(default_factory=lambda: int(os.getenv('NEXORADB_EXECUTION_WAIT_MS', '250')))
    execution_timeout_ms: int = field(default_factory=lambda: int(os.getenv('NEXORADB_EXECUTION_TIMEOUT_MS', '30000')))
    shutdown_drain_ms: int = field(default_factory=lambda: int(os.getenv('NEXORADB_SHUTDOWN_DRAIN_MS', '30000')))
    execution_memory_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_EXECUTION_MEMORY_MB', '0')))
    resource_process_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_RESOURCE_PROCESS_MB', '0')))
    resource_allocator_margin_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_RESOURCE_ALLOCATOR_MARGIN_MB', '128')))
    resource_python_margin_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_RESOURCE_PYTHON_MARGIN_MB', '128')))
    resource_page_cache_margin_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_RESOURCE_PAGE_CACHE_MARGIN_MB', '128')))
    resource_safety_margin_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_RESOURCE_SAFETY_MARGIN_MB', '64')))
    rocks_memory_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_ROCKS_MEMORY_MB', '256')))
    rocks_cache_mb: int = field(default_factory=lambda: int(os.getenv('NEXORADB_ROCKS_CACHE_MB', '0')))
    api_max_inflight: int = field(default_factory=lambda: int(os.getenv("NEXORADB_API_MAX_INFLIGHT", "16")))
    db_path: Path = field(
        default_factory=lambda: Path(os.getenv("NEXORADB_DB_PATH", "./nexoradb-data"))
    )
    graph_dir: Path = field(
        default_factory=lambda: Path(os.getenv("NEXORADB_GRAPH_DIR", "./graph_data"))
    )
    auth_secret: str = field(
        default_factory=lambda: os.getenv("NEXORADB_AUTH_SECRET", _DEFAULT_DEV_AUTH_SECRET)
    )
    api_token_secret: str = field(
        default_factory=lambda: os.getenv("NEXORADB_API_TOKEN_SECRET", _DEFAULT_DEV_API_TOKEN_SECRET)
    )
    access_token_ttl_seconds: int = field(
        default_factory=lambda: int(
            os.getenv("NEXORADB_ACCESS_TOKEN_TTL_SECONDS", str(_DEFAULT_ACCESS_TOKEN_TTL_SECONDS))
        )
    )
    allowed_origins: tuple[str, ...] = field(
        default_factory=lambda: tuple(
            _split_csv(
                os.getenv(
                    "NEXORADB_ADMIN_ORIGINS",
                    "http://localhost:5173,http://127.0.0.1:5173,"
                    "http://localhost:8000,http://127.0.0.1:8000",
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

    rocks_auto_tune: bool = field(default_factory=lambda: os.getenv('NEXORADB_ROCKS_AUTO_TUNE', 'true').lower() == 'true')
    rocks_durability: str = field(default_factory=lambda: os.getenv('NEXORADB_ROCKS_DURABILITY', 'durable').lower())

    def validate_for_startup(self) -> None:
        for name, maximum in (("execution_query_slots", 256), ("execution_per_app", 256),
                              ("execution_queue_size", 4096), ("execution_wait_ms", 5000),
                              ("execution_timeout_ms", 300000), ("shutdown_drain_ms", 300000)):
            if type(getattr(self, name)) is not int or not 1 <= getattr(self, name) <= maximum:
                raise RuntimeError(name + " is outside its supported positive range")
        if self.execution_query_slots < 2 or self.execution_per_app < 2:
            raise RuntimeError("execution query/app slots must leave one slot for small work (minimum 2)")
        if self.execution_queue_size < 3:
            raise RuntimeError("execution queue must leave entries for small work and maintenance (minimum 3)")
        if type(self.rocks_durability) is not str or self.rocks_durability not in {'durable', 'buffered'}:
            raise RuntimeError('NEXORADB_ROCKS_DURABILITY must be durable or buffered')
        if type(self.rocks_auto_tune) is not bool or os.getenv('NEXORADB_ROCKS_AUTO_TUNE', 'true').lower() not in {'true', 'false'}:
            raise RuntimeError('NEXORADB_ROCKS_AUTO_TUNE must be true or false')
        if (type(self.rocks_memory_mb) is not int or not 4 <= self.rocks_memory_mb <= 16384
                or type(self.rocks_cache_mb) is not int or not 0 <= self.rocks_cache_mb < self.rocks_memory_mb):
            raise RuntimeError('Rocks memory must be 4..16384 MiB; cache must be 0 (half) or below total budget')
        for name in ('execution_memory_mb', 'resource_process_mb', 'resource_allocator_margin_mb',
                     'resource_python_margin_mb', 'resource_page_cache_margin_mb', 'resource_safety_margin_mb'):
            if type(getattr(self, name)) is not int or not 0 <= getattr(self, name) <= 1048576:
                raise RuntimeError(name + ' must be 0..1048576 MiB')
        if not 1 <= self.api_max_inflight <= 1024:
            raise RuntimeError("NEXORADB_API_MAX_INFLIGHT must be 1..1024")
        if self.environment.lower() not in {"development", "test", "production"}:
            raise RuntimeError("NEXORADB_ENV must be development, test, or production")
        if self.environment.lower() == "production" and (
            self.auth_secret == _DEFAULT_DEV_AUTH_SECRET
            or self.api_token_secret == _DEFAULT_DEV_API_TOKEN_SECRET
        ):
            raise RuntimeError("Development signing secrets are forbidden in production")
        if self.environment.lower() == "production" and "NEXORADB_AUTH_SECRET" not in os.environ:
            raise RuntimeError("NEXORADB_AUTH_SECRET is required in production")
        if self.environment.lower() == "production" and "NEXORADB_API_TOKEN_SECRET" not in os.environ:
            raise RuntimeError("NEXORADB_API_TOKEN_SECRET is required in production")
        if len(self.auth_secret) < 32:
            raise RuntimeError("NEXORADB_AUTH_SECRET must be at least 32 characters")
        if len(self.api_token_secret) < 32:
            raise RuntimeError("NEXORADB_API_TOKEN_SECRET must be at least 32 characters")
