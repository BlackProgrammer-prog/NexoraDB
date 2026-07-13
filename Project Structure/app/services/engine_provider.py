from __future__ import annotations

from typing import Any, Optional

from app.core.config import settings
from app.core.native import create_doc_engine, create_graph_manager

_engine: Optional[Any] = None
_graph_manager: Optional[Any] = None


def get_engine() -> Any:
    """Get or create the DocEngine instance (Singleton)."""
    global _engine
    if _engine is None:
        _engine = create_doc_engine(settings)
    return _engine


def get_graph_manager() -> Any | None:
    """Get or create the GraphManager instance (Singleton)."""
    global _graph_manager
    if _graph_manager is None:
        _graph_manager = create_graph_manager(settings, get_engine())
    return _graph_manager


def reset_engine() -> None:
    """Reset engine instances (useful for testing)."""
    global _engine, _graph_manager
    _engine = None
    _graph_manager = None