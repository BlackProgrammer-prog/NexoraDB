from __future__ import annotations

import json
import re
import time
import uuid
from pathlib import Path
from typing import Any, Protocol

from fastapi import HTTPException, status
from pydantic import BaseModel, Field


class GraphManagerLike(Protocol):
    def create_graph(self, definition: Any) -> bool: ...

    def drop_graph(self, graph_name: str) -> bool: ...

    def list_graphs(self) -> list[str]: ...

    def get_stats(self, graph_name: str) -> Any: ...


class CreateGraphRequest(BaseModel):
    name: str = Field(min_length=1, max_length=128)
    description: str | None = None


class GraphNodeRequest(BaseModel):
    label: str = Field(min_length=1, max_length=128)
    type: str | None = None
    data: dict[str, Any] = Field(default_factory=dict)


class GraphEdgeRequest(BaseModel):
    source: str = Field(min_length=1)
    target: str = Field(min_length=1)
    label: str | None = None
    data: dict[str, Any] = Field(default_factory=dict)


def _now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _slug(value: str) -> str:
    normalized = re.sub(r"[^A-Za-z0-9_]+", "_", value.strip()).strip("_")
    if not normalized:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail={"message": "graph name must contain at least one letter or number"},
        )
    return normalized


class GraphMetadataStore:
    def __init__(self, graph_dir: Path) -> None:
        self.graph_dir = graph_dir
        self.path = graph_dir / "admin_graphs.json"

    def load(self) -> dict[str, dict[str, Any]]:
        if not self.path.exists():
            return {}
        try:
            value = json.loads(self.path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail={"message": "graph metadata file is invalid JSON"},
            ) from exc
        return value if isinstance(value, dict) else {}

    def save(self, graphs: dict[str, dict[str, Any]]) -> None:
        self.graph_dir.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(graphs, indent=2, sort_keys=True), encoding="utf-8")


def _make_graph_response(graph_id: str, metadata: dict[str, Any], stats: Any | None = None) -> dict[str, Any]:
    return {
        "id": graph_id,
        "name": metadata.get("name", graph_id),
        "description": metadata.get("description") or None,
        "nodes": metadata.get("nodes", []),
        "edges": metadata.get("edges", []),
        "createdAt": metadata.get("createdAt") or _now_iso(),
        "updatedAt": metadata.get("updatedAt") or _now_iso(),
        "stats": {
            "activeNodes": getattr(stats, "active_nodes", 0) if stats is not None else 0,
            "activeEdges": getattr(stats, "active_edges", 0) if stats is not None else 0,
            "version": getattr(stats, "version", 0) if stats is not None else 0,
        },
    }


def _get_metadata_or_404(store: GraphMetadataStore, graph_id: str) -> tuple[dict[str, dict[str, Any]], dict[str, Any]]:
    graphs = store.load()
    metadata = graphs.get(graph_id)
    if metadata is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail={"message": f"Graph '{graph_id}' not found"},
        )
    return graphs, metadata


def _create_native_definition(native: Any, graph_id: str) -> Any:
    definition = native.GraphDefinition()
    definition.name = graph_id
    definition.mode = native.GraphMode.Live
    definition.directed = True
    definition.heterogeneous = True
    definition.auto_build_on_startup = False
    return definition


def list_graphs(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
) -> list[dict[str, Any]]:
    metadata = metadata_store.load()
    native_graphs = set(graph_manager.list_graphs())
    changed = False

    for graph_id in native_graphs:
        if graph_id not in metadata:
            timestamp = _now_iso()
            metadata[graph_id] = {
                "name": graph_id,
                "description": None,
                "nodes": [],
                "edges": [],
                "createdAt": timestamp,
                "updatedAt": timestamp,
            }
            changed = True

    for graph_id in list(metadata):
        if graph_id not in native_graphs:
            metadata.pop(graph_id)
            changed = True

    if changed:
        metadata_store.save(metadata)

    return [
        _make_graph_response(graph_id, metadata[graph_id], _safe_stats(graph_manager, graph_id))
        for graph_id in sorted(native_graphs)
    ]


def create_graph(
    *,
    native: Any,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    payload: CreateGraphRequest,
) -> dict[str, Any]:
    graph_id = _slug(payload.name)
    graphs = metadata_store.load()
    if graph_id in graphs or graph_id in set(graph_manager.list_graphs()):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail={"message": f"Graph '{graph_id}' already exists"},
        )

    if not graph_manager.create_graph(_create_native_definition(native, graph_id)):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail={"message": "could not create graph in NexoraDB graph engine"},
        )

    timestamp = _now_iso()
    graphs[graph_id] = {
        "name": payload.name.strip(),
        "description": payload.description,
        "nodes": [],
        "edges": [],
        "createdAt": timestamp,
        "updatedAt": timestamp,
    }
    metadata_store.save(graphs)
    return _make_graph_response(graph_id, graphs[graph_id], _safe_stats(graph_manager, graph_id))


def delete_graph(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    graph_id: str,
) -> None:
    graphs = metadata_store.load()
    if not graph_manager.drop_graph(graph_id):
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail={"message": f"Graph '{graph_id}' not found"},
        )
    graphs.pop(graph_id, None)
    metadata_store.save(graphs)


def create_node(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    graph_id: str,
    payload: GraphNodeRequest,
) -> dict[str, Any]:
    graphs, graph = _get_metadata_or_404(metadata_store, graph_id)
    node = {
        "id": f"node_{uuid.uuid4().hex}",
        "label": payload.label,
        "type": payload.type,
        "data": payload.data,
    }
    graph["nodes"] = [*graph.get("nodes", []), node]
    graph["updatedAt"] = _now_iso()
    metadata_store.save(graphs)
    return _make_graph_response(graph_id, graph, _safe_stats(graph_manager, graph_id))


def update_node(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    graph_id: str,
    node_id: str,
    payload: GraphNodeRequest,
) -> dict[str, Any]:
    graphs, graph = _get_metadata_or_404(metadata_store, graph_id)
    nodes = graph.get("nodes", [])
    if not any(node.get("id") == node_id for node in nodes):
        raise HTTPException(status_code=404, detail={"message": f"Node '{node_id}' not found"})
    graph["nodes"] = [
        {
            **node,
            "label": payload.label,
            "type": payload.type,
            "data": payload.data,
        }
        if node.get("id") == node_id
        else node
        for node in nodes
    ]
    graph["updatedAt"] = _now_iso()
    metadata_store.save(graphs)
    return _make_graph_response(graph_id, graph, _safe_stats(graph_manager, graph_id))


def delete_node(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    graph_id: str,
    node_id: str,
) -> dict[str, Any]:
    graphs, graph = _get_metadata_or_404(metadata_store, graph_id)
    graph["nodes"] = [node for node in graph.get("nodes", []) if node.get("id") != node_id]
    graph["edges"] = [
        edge
        for edge in graph.get("edges", [])
        if edge.get("source") != node_id and edge.get("target") != node_id
    ]
    graph["updatedAt"] = _now_iso()
    metadata_store.save(graphs)
    return _make_graph_response(graph_id, graph, _safe_stats(graph_manager, graph_id))


def create_edge(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    graph_id: str,
    payload: GraphEdgeRequest,
) -> dict[str, Any]:
    graphs, graph = _get_metadata_or_404(metadata_store, graph_id)
    node_ids = {node.get("id") for node in graph.get("nodes", [])}
    if payload.source not in node_ids or payload.target not in node_ids:
        raise HTTPException(status_code=400, detail={"message": "edge source and target must exist"})
    edge = {
        "id": f"edge_{uuid.uuid4().hex}",
        "source": payload.source,
        "target": payload.target,
        "label": payload.label,
        "data": payload.data,
    }
    graph["edges"] = [*graph.get("edges", []), edge]
    graph["updatedAt"] = _now_iso()
    metadata_store.save(graphs)
    return _make_graph_response(graph_id, graph, _safe_stats(graph_manager, graph_id))


def update_edge(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    graph_id: str,
    edge_id: str,
    payload: GraphEdgeRequest,
) -> dict[str, Any]:
    graphs, graph = _get_metadata_or_404(metadata_store, graph_id)
    edges = graph.get("edges", [])
    if not any(edge.get("id") == edge_id for edge in edges):
        raise HTTPException(status_code=404, detail={"message": f"Edge '{edge_id}' not found"})
    graph["edges"] = [
        {
            **edge,
            "source": payload.source,
            "target": payload.target,
            "label": payload.label,
            "data": payload.data,
        }
        if edge.get("id") == edge_id
        else edge
        for edge in edges
    ]
    graph["updatedAt"] = _now_iso()
    metadata_store.save(graphs)
    return _make_graph_response(graph_id, graph, _safe_stats(graph_manager, graph_id))


def delete_edge(
    *,
    graph_manager: GraphManagerLike,
    metadata_store: GraphMetadataStore,
    graph_id: str,
    edge_id: str,
) -> dict[str, Any]:
    graphs, graph = _get_metadata_or_404(metadata_store, graph_id)
    graph["edges"] = [edge for edge in graph.get("edges", []) if edge.get("id") != edge_id]
    graph["updatedAt"] = _now_iso()
    metadata_store.save(graphs)
    return _make_graph_response(graph_id, graph, _safe_stats(graph_manager, graph_id))


def _safe_stats(graph_manager: GraphManagerLike, graph_id: str) -> Any | None:
    try:
        return graph_manager.get_stats(graph_id)
    except Exception:
        return None
