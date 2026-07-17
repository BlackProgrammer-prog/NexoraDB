from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest
from fastapi import HTTPException


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "NexoraDB" / "src"))

from nexoradb_admin.graph_store import (  # noqa: E402
    GraphMetadataStore,
    get_graph_node_document,
    get_graph_visualization,
)


class FakeMapping:
    node_type = "User"
    collection = "users"


class FakeDefinition:
    node_mappings = [FakeMapping()]


class FakeResult:
    success = True
    data = json.dumps({"_id": "u1", "name": "Parham", "age": 30})


class FakeEngine:
    def find_by_id(self, collection: str, node_id: str) -> FakeResult:
        assert collection == "users"
        assert node_id == "u1"
        return FakeResult()


class FakeSnapshot:
    def __init__(self, node_count: int = 2) -> None:
        self._node_count = node_count

    def node_count(self) -> int:
        return self._node_count

    def for_each_node(self, callback) -> None:
        for dense_id in range(self._node_count):
            if callback(dense_id, 1) is False:
                break

    def for_each_edge(self, callback) -> None:
        if self._node_count >= 2:
            callback(7, 0, 1, 2)

    def node_type_name(self, _: int) -> str:
        return "User"

    def edge_type_name(self, _: int) -> str:
        return "FOLLOWS"

    def ext_id(self, dense_id: int) -> str:
        return f"u{dense_id + 1}"

    def dense_id(self, node_id: str) -> int:
        return 0 if node_id == "u1" else (1 << 64) - 1

    def has_node(self, dense_id: int) -> bool:
        return dense_id == 0

    def node_type(self, _: int) -> int:
        return 1


class FakeGraphManager:
    def __init__(self, node_count: int = 2) -> None:
        self.snapshot = FakeSnapshot(node_count)

    def create_snapshot(self, _: str) -> FakeSnapshot:
        return self.snapshot

    def get_definition(self, _: str) -> FakeDefinition:
        return FakeDefinition()


def metadata_store(tmp_path: Path) -> GraphMetadataStore:
    store = GraphMetadataStore(tmp_path)
    store.save(
        {
            "social": {
                "name": "Social",
                "nodes": [],
                "edges": [],
                "createdAt": "2026-07-17T00:00:00Z",
                "updatedAt": "2026-07-17T00:00:00Z",
            }
        }
    )
    return store


def test_visualization_exports_snapshot_topology(tmp_path: Path) -> None:
    result = get_graph_visualization(
        graph_manager=FakeGraphManager(),
        metadata_store=metadata_store(tmp_path),
        graph_id="social",
    )

    assert result["nodeCount"] == 2
    assert result["edgeCount"] == 1
    assert result["nodes"][0] == {
        "id": "u1",
        "label": "u1",
        "type": "User",
        "collection": "users",
    }
    assert result["edges"][0]["label"] == "FOLLOWS"


def test_visualization_rejects_more_than_one_thousand_nodes(tmp_path: Path) -> None:
    with pytest.raises(HTTPException) as error:
        get_graph_visualization(
            graph_manager=FakeGraphManager(node_count=1_001),
            metadata_store=metadata_store(tmp_path),
            graph_id="social",
        )

    assert error.value.status_code == 422


def test_node_click_loads_source_document(tmp_path: Path) -> None:
    result = get_graph_node_document(
        engine=FakeEngine(),
        graph_manager=FakeGraphManager(),
        metadata_store=metadata_store(tmp_path),
        graph_id="social",
        node_id="u1",
    )

    assert result["collection"] == "users"
    assert result["nodeType"] == "User"
    assert result["document"]["name"] == "Parham"
