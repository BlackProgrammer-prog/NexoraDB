from __future__ import annotations

from collections.abc import Callable
from typing import Any

from fastapi import Depends, FastAPI, Header, HTTPException, Request, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from .config import AdminApiSettings
from .monitoring import MonitoringSocketServer, MonitoringState
from .models import (
    AdminRegisterRequest,
    AuthResponse,
    LoginRequest,
    MessageResponse,
    PublicUser,
    SetupStateResponse,
)
from .native import create_doc_engine
from .security import TokenError, decode_access_token
from .service import (
    InternalUserEngine,
    get_current_public_user,
    login_admin,
    register_root_admin,
    root_exists,
)


def create_app(
    *,
    settings: AdminApiSettings | None = None,
    engine: InternalUserEngine | None = None,
    engine_factory: Callable[[AdminApiSettings], InternalUserEngine] | None = None,
    monitoring_state: MonitoringState | None = None,
) -> FastAPI:
    app_settings = settings or AdminApiSettings()
    app_settings.validate_for_startup()
    resolved_engine_factory = engine_factory or create_doc_engine

    app = FastAPI(
        title="NexoraDB Admin API",
        version="0.1.0",
        docs_url="/docs" if app_settings.environment != "production" else None,
        redoc_url=None,
    )
    app.add_middleware(
        CORSMiddleware,
        allow_origins=list(app_settings.allowed_origins),
        allow_credentials=False,
        allow_methods=["GET", "POST"],
        allow_headers=["Authorization", "Content-Type"],
    )
    app.state.settings = app_settings
    app.state.engine = engine
    app.state.monitoring_state = monitoring_state or MonitoringState()

    @app.exception_handler(HTTPException)
    async def http_exception_handler(_: Request, exc: HTTPException) -> JSONResponse:
        if isinstance(exc.detail, dict) and "message" in exc.detail:
            return JSONResponse(status_code=exc.status_code, content=exc.detail)
        if isinstance(exc.detail, str):
            return JSONResponse(status_code=exc.status_code, content={"message": exc.detail})
        return JSONResponse(status_code=exc.status_code, content={"message": "request failed"})

    def get_settings() -> AdminApiSettings:
        return app.state.settings

    def get_engine() -> InternalUserEngine:
        if app.state.engine is None:
            app.state.engine = resolved_engine_factory(app.state.settings)
        return app.state.engine

    def get_token_payload_from_header(authorization: str | None) -> dict[str, Any] | None:
        if authorization is None or not authorization.startswith("Bearer "):
            return None
        try:
            return decode_access_token(
                authorization.removeprefix("Bearer ").strip(),
                app.state.settings,
            )
        except TokenError:
            return None

    def current_token_payload(
        authorization: str | None = Header(default=None),
        current_settings: AdminApiSettings = Depends(get_settings),
    ) -> dict[str, Any]:
        if authorization is None or not authorization.startswith("Bearer "):
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail={"message": "missing bearer token"},
            )
        try:
            return decode_access_token(authorization.removeprefix("Bearer ").strip(), current_settings)
        except TokenError as exc:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail={"message": str(exc)},
            ) from exc

    @app.middleware("http")
    async def record_monitoring_request(request: Request, call_next: Callable[..., Any]) -> Any:
        response = await call_next(request)
        payload = get_token_payload_from_header(request.headers.get("authorization"))
        client_host = request.client.host if request.client else "unknown"
        user = str(payload["sub"]) if payload and payload.get("sub") else None
        client_id = f"http:{user or client_host}"
        await app.state.monitoring_state.record_request(
            client_id=client_id,
            address=client_host,
            user=user,
            kind="http",
        )
        return response

    @app.get("/health", response_model=MessageResponse)
    def health() -> MessageResponse:
        return MessageResponse(message="ok")

    @app.get("/auth/setup-state", response_model=SetupStateResponse)
    def setup_state(current_engine: InternalUserEngine = Depends(get_engine)) -> dict[str, bool]:
        return {"needsSetup": not root_exists(current_engine)}

    @app.post(
        "/auth/register",
        response_model=PublicUser,
        status_code=status.HTTP_201_CREATED,
    )
    def register(
        payload: AdminRegisterRequest,
        current_engine: InternalUserEngine = Depends(get_engine),
    ) -> PublicUser:
        return register_root_admin(current_engine, payload)

    @app.post("/auth/login", response_model=AuthResponse)
    def login(
        payload: LoginRequest,
        current_engine: InternalUserEngine = Depends(get_engine),
        current_settings: AdminApiSettings = Depends(get_settings),
    ) -> AuthResponse:
        return login_admin(current_engine, payload.username, payload.password, current_settings)

    @app.get("/auth/me", response_model=PublicUser)
    def me(
        token_payload: dict[str, Any] = Depends(current_token_payload),
        current_engine: InternalUserEngine = Depends(get_engine),
    ) -> PublicUser:
        return get_current_public_user(current_engine, str(token_payload["sub"]))

    return app


def create_asgi_app(
    *,
    settings: AdminApiSettings | None = None,
    engine: InternalUserEngine | None = None,
    engine_factory: Callable[[AdminApiSettings], InternalUserEngine] | None = None,
) -> Any:
    app_settings = settings or AdminApiSettings()
    monitoring_state = MonitoringState()
    fastapi_app = create_app(
        settings=app_settings,
        engine=engine,
        engine_factory=engine_factory,
        monitoring_state=monitoring_state,
    )

    def get_engine_for_monitoring() -> Any:
        if fastapi_app.state.engine is None:
            fastapi_app.state.engine = (engine_factory or create_doc_engine)(app_settings)
        return fastapi_app.state.engine

    monitoring_server = MonitoringSocketServer(
        settings=app_settings,
        state=monitoring_state,
        engine_provider=get_engine_for_monitoring,
    )
    fastapi_app.state.monitoring_server = monitoring_server
    return monitoring_server.asgi_app(fastapi_app)


app = create_asgi_app()
