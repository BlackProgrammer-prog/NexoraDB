from __future__ import annotations

import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.api.v1.endpoints import (
    query, documents, collections, graph, algorithms, apps, system
)
from app.core.config import settings
from app.services.engine_provider import get_engine

logging.basicConfig(
    level=settings.LOG_LEVEL,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Application lifespan manager."""
    # Startup
    logger.info(f"Starting {settings.APP_NAME} v{settings.APP_VERSION}")
    logger.info(f"Environment: {settings.ENVIRONMENT}")
    logger.info(f"DB Path: {settings.DB_PATH}")
    
    # Validate settings
    settings.validate_for_startup()
    
    # Initialize engine (lazy)
    if settings.ENVIRONMENT != "development":
        logger.info("Initializing DocEngine...")
        engine = get_engine()
        if not engine.IsHealthy():
            logger.error("DocEngine is not healthy!")
    
    logger.info("API ready")
    
    yield
    
    # Shutdown
    logger.info("Shutting down...")


# Create FastAPI app
app = FastAPI(
    title=settings.APP_NAME,
    description="NexoraDB External API - Document Store, Graph Engine, and Algorithms",
    version=settings.APP_VERSION,
    lifespan=lifespan,
    docs_url="/docs" if settings.ENVIRONMENT != "production" else None,
    redoc_url="/redoc" if settings.ENVIRONMENT != "production" else None,
)

# Store settings in app state
app.state.settings = settings

# CORS middleware
app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.ALLOWED_ORIGINS,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Register routers
app.include_router(query.router, prefix="/api/v1")
app.include_router(documents.router, prefix="/api/v1")
app.include_router(collections.router, prefix="/api/v1")
app.include_router(graph.router, prefix="/api/v1")
app.include_router(algorithms.router, prefix="/api/v1")
app.include_router(apps.router, prefix="/api/v1")
app.include_router(system.router, prefix="/api/v1")


@app.get("/", response_model=dict)
async def root():
    return {
        "app": settings.APP_NAME,
        "version": settings.APP_VERSION,
        "environment": settings.ENVIRONMENT,
        "docs": "/docs" if settings.ENVIRONMENT != "production" else None,
        "status": "running"
    }


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "app.main:app",
        host=settings.HOST,
        port=settings.PORT,
        reload=settings.DEBUG,
        log_level=settings.LOG_LEVEL.lower(),
    )