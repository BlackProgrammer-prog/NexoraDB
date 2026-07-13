from __future__ import annotations

import importlib
import sys
from typing import Any, Optional

from app.core.config import Settings


def load_native_module(settings: Settings) -> Any:
    try:
        if settings.NATIVE_MODULE_PATH:
            sys.path.insert(0, settings.NATIVE_MODULE_PATH)
        
        module_names = ["nexoradb", "nexoradb_native", "_nexoradb"]
        for name in module_names:
            try:
                return importlib.import_module(name)
            except ImportError:
                continue
        
        if settings.ENVIRONMENT == "development":
            return _create_mock_module()
        
        raise ImportError("Could not load native C++ module")
    except ImportError as e:
        if settings.ENVIRONMENT == "production":
            raise
        return _create_mock_module()


def create_doc_engine(settings: Settings) -> Any:
    native = load_native_module(settings)
    if hasattr(native, "DocEngine"):
        try:
            return native.DocEngine(settings.ROCKSDB_PATH)
        except Exception as e:
            if settings.ENVIRONMENT == "production":
                raise
            return _MockDocEngine()
    return _MockDocEngine()


def create_graph_manager(settings: Settings, engine: Any) -> Any | None:
    if not settings.GRAPH_ENABLED:
        return None
    native = load_native_module(settings)
    if hasattr(native, "GraphManager"):
        try:
            return native.GraphManager(engine, settings.GRAPH_PATH)
        except Exception:
            if settings.ENVIRONMENT == "production":
                raise
            return _MockGraphManager()
    return _MockGraphManager()


class _MockDocEngine:
    def __init__(self, *args, **kwargs):
        self._collections = {}
    
    def IsHealthy(self) -> bool:
        return True
    
    def CreateCollection(self, name: str, schema=None):
        if name in self._collections:
            return {"success": False, "error_msg": "Collection exists"}
        self._collections[name] = {"docs": {}}
        return {"success": True, "data": f"Collection '{name}' created"}
    
    def InsertOne(self, collection: str, doc: dict):
        import uuid
        doc_id = doc.get("_id", str(uuid.uuid4()))
        if collection not in self._collections:
            return {"success": False, "error_msg": "Collection not found"}
        self._collections[collection]["docs"][doc_id] = doc
        return {"success": True, "data": doc_id}
    
    def FindById(self, collection: str, doc_id: str):
        if collection not in self._collections:
            return {"success": False, "error_msg": "Collection not found"}
        doc = self._collections[collection]["docs"].get(doc_id)
        if not doc:
            return {"success": False, "error_msg": "Document not found"}
        return {"success": True, "data": doc}
    
    def ListCollections(self):
        return list(self._collections.keys())


class _MockGraphManager:
    def __init__(self, *args, **kwargs):
        self._nodes = {}
    
    def addNode(self, ext_id: str, type_name: str):
        return 1
    
    def neighborsExt(self, ext_id: str, direction: str = "out", edge_type: str = "", limit: int = 100):
        return []
    
    def stats(self):
        return {"active_nodes": 0, "active_edges": 0, "version": 1}


def _create_mock_module():
    import types
    module = types.ModuleType("nexoradb")
    module.DocEngine = _MockDocEngine
    module.GraphManager = _MockGraphManager
    module.GRAPH_ENABLED = True
    return module
    