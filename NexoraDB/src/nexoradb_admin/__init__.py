"""FastAPI backend for the NexoraDB admin dashboard."""

from .app import app, create_app

__all__ = ["app", "create_app"]
