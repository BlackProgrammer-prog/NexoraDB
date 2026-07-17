"""End-to-end NexoraQL query-language scenarios.

These tests execute real NexoraQL text through Executor.execute_text().  The
DocEngine/GraphManager are in-memory fakes so the query-language contract can be
tested without requiring the compiled nexoradb pybind module.
"""

from __future__ import annotations

import copy
import json
import re
import sys
import unittest
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "NexoraDB" / "src"
sys.path.insert(0, str(SRC))

from nexoradb.parser import semantic as S  # noqa: E402
from nexoradb.parser.semantic import Executor  # noqa: E402


@dataclass
class DBResult:
    success: bool
    data: str = ""
    error_msg: str = ""


@dataclass
class BuildResult:
    success: bool
    nodes_built: int = 0
    edges_built: int = 0
    elapsed_ms: float = 0.0
    error_msg: str = ""


@dataclass
class AlgoResult:
    success: bool
    error_msg: str = ""
    result_json: str = "{}"
    elapsed_ms: float = 0.0


@dataclass
class GraphStats:
    active_nodes: int = 0
    active_edges: int = 0
    deleted_nodes: int = 0
    deleted_edges: int = 0
    heavy_nodes: int = 0
    version: int = 1


class FakeFieldType:
    String = "String"
    Int32 = "Int32"
    Int64 = "Int64"
    Float64 = "Float64"
    Bool = "Bool"
    Array = "Array"
    Object = "Object"
    Binary = "Binary"


class FakeIndexType:
    SingleField = "SingleField"
    Compound = "Compound"
    Unique = "Unique"


class FakeGraphMode:
    Live = "Live"
    Static = "Static"


class FakeOp:
    EQ = "EQ"
    NEQ = "NEQ"
    GT = "GT"
    GTE = "GTE"
    LT = "LT"
    LTE = "LTE"
    EXISTS = "EXISTS"
    REGEX = "REGEX"
    STARTS = "STARTS"
    CONTAINS = "CONTAINS"


class FakeValueType:
    String = "String"
    Int64 = "Int64"
    Float64 = "Float64"
    Bool = "Bool"
    Null = "Null"


class FakeUpdateValueType:
    String = "String"
    Int64 = "Int64"
    Float64 = "Float64"
    Bool = "Bool"
    Null = "Null"
    Array = "Array"


class SchemaField:
    def __init__(self):
        self.name = ""
        self.type = FakeFieldType.String
        self.required = False
        self.unique = False


class SchemaDefinition:
    def __init__(self):
        self.fields: list[SchemaField] = []
        self.strict = False


class IndexDefinition:
    def __init__(self):
        self.index_name = ""
        self.fields: list[str] = []
        self.type = FakeIndexType.SingleField


class ForeignKeyDefinition:
    def __init__(self):
        self.fk_name = ""
        self.local_field = ""
        self.ref_collection = ""
        self.ref_field = ""


class GraphDefinition:
    def __init__(self):
        self.name = ""
        self.mode = FakeGraphMode.Live
        self.directed = True
        self.heterogeneous = True
        self.auto_build_on_startup = True
        self.node_mappings: list[NodeMappingDef] = []
        self.edge_mappings: list[EdgeMappingDef] = []


class NodeMappingDef:
    def __init__(self):
        self.node_type = ""
        self.collection = ""
        self.key_path = ""
        self.properties: list[str] = []
        self.filter_expr = ""


class EdgeMappingDef:
    def __init__(self):
        self.edge_type = ""
        self.collection = ""
        self.source_path = ""
        self.source_node_type = ""
        self.target_path = ""
        self.target_node_type = ""
        self.directed = True
        self.properties: list[str] = []
        self.unwind = None


class UnwindConfig:
    def __init__(self):
        self.array_path = ""
        self.alias = ""


class Condition:
    def __init__(self, kind: str = "empty", **kwargs):
        self.kind = kind
        self.__dict__.update(kwargs)

    @staticmethod
    def leaf(field: str, op: str, value: str, value_type: str = FakeValueType.String):
        return Condition("leaf", field=field, op=op, value=value, value_type=value_type)

    @staticmethod
    def in_(field: str, values: list[str], negate: bool = False):
        return Condition("in", field=field, values=values, negate=negate)

    @staticmethod
    def and_(conditions: list["Condition"]):
        return Condition("composite", logic="AND", sub_conditions=conditions)

    @staticmethod
    def or_(conditions: list["Condition"]):
        return Condition("composite", logic="OR", sub_conditions=conditions)

    @staticmethod
    def nor_(conditions: list["Condition"]):
        return Condition("composite", logic="NOR", sub_conditions=conditions)


class UpdateSpec:
    def __init__(self):
        self.operations: list[tuple] = []
        self.upsert = False

    def set(self, field: str, value: str, value_type: str = FakeUpdateValueType.String):
        self.operations.append(("set", field, value, value_type))
        return self

    def unset(self, field: str):
        self.operations.append(("unset", field))
        return self

    def inc(self, field: str, delta: str, value_type: str = FakeUpdateValueType.Int64):
        self.operations.append(("inc", field, delta, value_type))
        return self

    def push(self, field: str, element: str, value_type: str = FakeUpdateValueType.String):
        self.operations.append(("push", field, element, value_type))
        return self

    def pull(self, field: str, element: str, value_type: str = FakeUpdateValueType.String):
        self.operations.append(("pull", field, element, value_type))
        return self

    def touch_date(self, field: str):
        self.operations.append(("touch_date", field))
        return self


class FakeNx:
    FieldType = FakeFieldType
    IndexType = FakeIndexType
    GraphMode = FakeGraphMode
    Op = FakeOp
    ValueType = FakeValueType
    UpdateValueType = FakeUpdateValueType
    SchemaField = SchemaField
    SchemaDefinition = SchemaDefinition
    SchemaFieldList = list
    IndexDefinition = IndexDefinition
    ForeignKeyDefinition = ForeignKeyDefinition
    GraphDefinition = GraphDefinition
    NodeMappingDef = NodeMappingDef
    EdgeMappingDef = EdgeMappingDef
    UnwindConfig = UnwindConfig
    Condition = Condition
    UpdateSpec = UpdateSpec
    GRAPH_ENABLED = True
    __version__ = "test"


def get_path(doc: dict, path: str) -> Any:
    cur: Any = doc
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def set_path(doc: dict, path: str, value: Any) -> None:
    parts = path.split(".")
    cur = doc
    for part in parts[:-1]:
        cur = cur.setdefault(part, {})
    cur[parts[-1]] = value


def unset_path(doc: dict, path: str) -> None:
    parts = path.split(".")
    cur = doc
    for part in parts[:-1]:
        cur = cur.get(part, {})
    if isinstance(cur, dict):
        cur.pop(parts[-1], None)


def coerce(raw: str, value_type: str) -> Any:
    if value_type == FakeValueType.Int64:
        return int(raw)
    if value_type == FakeValueType.Float64:
        return float(raw)
    if value_type == FakeValueType.Bool:
        return raw in ("1", "true", "True")
    if value_type == FakeValueType.Null:
        return None
    return raw


def match_condition(doc: dict, cond: Condition) -> bool:
    if cond.kind == "empty":
        return True
    if cond.kind == "composite":
        subs = [match_condition(doc, sub) for sub in cond.sub_conditions]
        if cond.logic == "AND":
            return all(subs)
        if cond.logic == "OR":
            return any(subs)
        if cond.logic == "NOR":
            return not any(subs)
    if cond.kind == "in":
        actual = get_path(doc, cond.field)
        ok = str(actual) in set(cond.values)
        return not ok if cond.negate else ok

    actual = get_path(doc, cond.field)
    if cond.op == FakeOp.EXISTS:
        exists = actual is not None
        return exists if cond.value == "1" else not exists

    expected = coerce(cond.value, cond.value_type)
    if actual is None:
        actual = None
    elif cond.value_type == FakeValueType.Int64:
        actual = int(actual)
    elif cond.value_type == FakeValueType.Float64:
        actual = float(actual)
    elif cond.value_type == FakeValueType.Bool:
        actual = bool(actual)
    else:
        actual = str(actual)

    if cond.op == FakeOp.EQ:
        return actual == expected
    if cond.op == FakeOp.NEQ:
        return actual != expected
    if cond.op == FakeOp.GT:
        return actual > expected
    if cond.op == FakeOp.GTE:
        return actual >= expected
    if cond.op == FakeOp.LT:
        return actual < expected
    if cond.op == FakeOp.LTE:
        return actual <= expected
    if cond.op == FakeOp.REGEX:
        return re.search(str(expected), str(actual)) is not None
    if cond.op == FakeOp.STARTS:
        return str(actual).startswith(str(expected))
    if cond.op == FakeOp.CONTAINS:
        return str(expected) in str(actual)
    raise AssertionError(f"unsupported fake op: {cond.op}")


def apply_update(doc: dict, spec: UpdateSpec) -> dict:
    out = copy.deepcopy(doc)
    for op in spec.operations:
        name = op[0]
        if name == "set":
            _, field, raw, value_type = op
            value = coerce(raw, value_type)
            if value_type == FakeUpdateValueType.Bool:
                value = raw == "true"
            set_path(out, field, value)
        elif name == "unset":
            _, field = op
            unset_path(out, field)
        elif name == "inc":
            _, field, raw, value_type = op
            current = get_path(out, field) or 0
            delta = float(raw) if value_type == FakeUpdateValueType.Float64 else int(raw)
            set_path(out, field, current + delta)
        elif name == "push":
            _, field, raw, value_type = op
            arr = list(get_path(out, field) or [])
            arr.append(coerce(raw, value_type))
            set_path(out, field, arr)
        elif name == "pull":
            _, field, raw, value_type = op
            value = coerce(raw, value_type)
            arr = [item for item in list(get_path(out, field) or []) if item != value]
            set_path(out, field, arr)
        elif name == "touch_date":
            _, field = op
            set_path(out, field, 1782560000000)
    return out


class InMemoryDocEngine:
    def __init__(self):
        self.collections: dict[str, dict[str, dict]] = {}
        self.schemas: dict[str, SchemaDefinition] = {}
        self.indexes: dict[str, dict[str, IndexDefinition]] = defaultdict(dict)
        self.foreign_keys: dict[str, dict[str, ForeignKeyDefinition]] = defaultdict(dict)
        self._tx_snapshot: dict[str, dict[str, dict]] | None = None

    def is_healthy(self) -> bool:
        return True

    def create_collection(self, name: str, schema: SchemaDefinition | None = None) -> DBResult:
        if name in self.collections:
            return DBResult(False, error_msg=f"collection exists: {name}")
        self.collections[name] = {}
        if schema is not None:
            self.schemas[name] = schema
        return DBResult(True)

    def drop_collection(self, name: str) -> DBResult:
        self.collections.pop(name, None)
        self.schemas.pop(name, None)
        return DBResult(True)

    def collection_exists(self, name: str) -> bool:
        return name in self.collections

    def list_collections(self) -> list[str]:
        return sorted(self.collections)

    def set_schema(self, collection: str, schema: SchemaDefinition) -> DBResult:
        self.schemas[collection] = schema
        return DBResult(True)

    def get_schema(self, collection: str) -> SchemaDefinition | None:
        return self.schemas.get(collection)

    def create_index(self, collection: str, index_def: IndexDefinition) -> DBResult:
        self.indexes[collection][index_def.index_name] = index_def
        return DBResult(True)

    def drop_index(self, collection: str, index_name: str) -> DBResult:
        self.indexes[collection].pop(index_name, None)
        return DBResult(True)

    def get_indexes(self, collection: str) -> list[IndexDefinition]:
        return list(self.indexes[collection].values())

    def add_foreign_key(self, collection: str, fk_def: ForeignKeyDefinition) -> DBResult:
        self.foreign_keys[collection][fk_def.fk_name] = fk_def
        return DBResult(True)

    def drop_foreign_key(self, collection: str, fk_name: str) -> DBResult:
        self.foreign_keys[collection].pop(fk_name, None)
        return DBResult(True)

    def get_foreign_keys(self, collection: str) -> list[ForeignKeyDefinition]:
        return list(self.foreign_keys[collection].values())

    def insert_one(self, collection: str, json_doc: str) -> DBResult:
        return self._insert_into(self.collections, collection, json_doc)

    def insert_many(self, collection: str, docs: list[str]) -> DBResult:
        ids = []
        for doc in docs:
            result = self.insert_one(collection, doc)
            if not result.success:
                return result
            ids.append(result.data)
        return DBResult(True, json.dumps(ids))

    def insert_one_tx(self, tx, collection: str, json_doc: str) -> DBResult:
        return self._insert_into(tx.data, collection, json_doc)

    def _insert_into(self, store: dict[str, dict[str, dict]], collection: str, json_doc: str) -> DBResult:
        if collection not in store:
            return DBResult(False, error_msg=f"missing collection: {collection}")
        doc = json.loads(json_doc)
        doc_id = str(doc.get("_id", f"{collection}_{len(store[collection]) + 1}"))
        doc["_id"] = doc_id
        store[collection][doc_id] = doc
        return DBResult(True, doc_id)

    def find_by_id(self, collection: str, doc_id: str) -> DBResult:
        doc = self.collections.get(collection, {}).get(doc_id)
        if doc is None:
            return DBResult(False, error_msg="not found")
        return DBResult(True, json.dumps(doc))

    def find_many(self, collection: str, condition: Condition | None = None,
                  limit: int = 0, skip: int = 0) -> DBResult:
        docs = [
            copy.deepcopy(doc)
            for doc in self.collections.get(collection, {}).values()
            if match_condition(doc, condition or Condition())
        ]
        docs = docs[skip:]
        if limit:
            docs = docs[:limit]
        return DBResult(True, json.dumps(docs))

    def count(self, collection: str, condition: Condition | None = None) -> DBResult:
        docs = json.loads(self.find_many(collection, condition).data)
        return DBResult(True, str(len(docs)))

    def exists(self, collection: str, condition: Condition) -> DBResult:
        docs = json.loads(self.find_many(collection, condition, limit=1).data)
        return DBResult(True, "true" if docs else "false")

    def update_by_id(self, collection: str, doc_id: str, spec: UpdateSpec) -> DBResult:
        if doc_id not in self.collections.get(collection, {}):
            return DBResult(False, error_msg="not found")
        self.collections[collection][doc_id] = apply_update(
            self.collections[collection][doc_id], spec)
        return DBResult(True, "1")

    def update_many(self, collection: str, condition: Condition, spec: UpdateSpec) -> DBResult:
        updated = 0
        for doc_id, doc in list(self.collections.get(collection, {}).items()):
            if match_condition(doc, condition):
                self.collections[collection][doc_id] = apply_update(doc, spec)
                updated += 1
        return DBResult(True, str(updated))

    def delete_by_id(self, collection: str, doc_id: str) -> DBResult:
        deleted = self.collections.get(collection, {}).pop(doc_id, None) is not None
        return DBResult(True, "1" if deleted else "0")

    def delete_many(self, collection: str, condition: Condition) -> DBResult:
        to_delete = [
            doc_id for doc_id, doc in self.collections.get(collection, {}).items()
            if match_condition(doc, condition)
        ]
        for doc_id in to_delete:
            self.collections[collection].pop(doc_id)
        return DBResult(True, str(len(to_delete)))

    def lookup_join(self, from_col: str, from_field: str, to_col: str, to_field: str,
                    condition: Condition, limit: int) -> dict:
        base_docs = json.loads(self.find_many(from_col, condition, limit).data)
        joined = []
        for doc in base_docs:
            match_value = get_path(doc, from_field)
            doc["__joined__"] = [
                copy.deepcopy(target)
                for target in self.collections.get(to_col, {}).values()
                if get_path(target, to_field) == match_value
            ]
            joined.append(json.dumps(doc))
        return {"success": True, "records": joined, "error_msg": ""}

    def get_collection_size(self, collection: str) -> int:
        return len(self.collections.get(collection, {}))

    def begin_transaction(self):
        self._tx_snapshot = copy.deepcopy(self.collections)
        return TxHandle(copy.deepcopy(self.collections))

    def commit_transaction(self, tx) -> DBResult:
        self.collections = tx.data
        self._tx_snapshot = None
        return DBResult(True)

    def rollback_transaction(self, tx) -> DBResult:
        self._tx_snapshot = None
        return DBResult(True)


class TxHandle:
    def __init__(self, data: dict[str, dict[str, dict]]):
        self.data = data

    def is_valid(self) -> bool:
        return True


class InMemoryGraphManager:
    def __init__(self, engine: InMemoryDocEngine):
        self.engine = engine
        self.graph_defs: dict[str, GraphDefinition] = {}
        self.node_maps: dict[str, list[NodeMappingDef]] = defaultdict(list)
        self.edge_maps: dict[str, list[EdgeMappingDef]] = defaultdict(list)
        self.nodes: dict[str, dict[str, dict]] = defaultdict(dict)
        self.edges: dict[str, list[dict]] = defaultdict(list)

    def startup(self) -> bool:
        return True

    def shutdown(self) -> None:
        return None

    def create_graph(self, definition: GraphDefinition) -> bool:
        self.graph_defs[definition.name] = definition
        return True

    def add_node_mapping(self, graph_name: str, mapping: NodeMappingDef) -> bool:
        self.node_maps[graph_name].append(mapping)
        self.graph_defs[graph_name].node_mappings.append(mapping)
        return True

    def add_edge_mapping(self, graph_name: str, mapping: EdgeMappingDef) -> bool:
        self.edge_maps[graph_name].append(mapping)
        self.graph_defs[graph_name].edge_mappings.append(mapping)
        return True

    def get_definition(self, graph_name: str) -> GraphDefinition | None:
        return self.graph_defs.get(graph_name)

    def drop_graph(self, graph_name: str) -> bool:
        self.graph_defs.pop(graph_name, None)
        self.nodes.pop(graph_name, None)
        self.edges.pop(graph_name, None)
        return True

    def list_graphs(self) -> list[str]:
        return sorted(self.graph_defs)

    def is_ready(self, graph_name: str) -> bool:
        return graph_name in self.nodes

    def build_graph(self, graph_name: str) -> BuildResult:
        nodes: dict[str, dict] = {}
        edges: list[dict] = []

        for mapping in self.node_maps[graph_name]:
            for doc in self.engine.collections.get(mapping.collection, {}).values():
                node_id = str(get_path(doc, mapping.key_path))
                nodes[node_id] = {"id": node_id, "type": mapping.node_type, "doc": copy.deepcopy(doc)}

        for mapping in self.edge_maps[graph_name]:
            for doc in self.engine.collections.get(mapping.collection, {}).values():
                src = str(get_path(doc, mapping.source_path))
                dst = str(get_path(doc, mapping.target_path))
                edges.append({
                    "src": src,
                    "dst": dst,
                    "type": mapping.edge_type,
                    "directed": mapping.directed,
                })

        self.nodes[graph_name] = nodes
        self.edges[graph_name] = edges
        return BuildResult(True, nodes_built=len(nodes), edges_built=len(edges))

    def render_graph(self, graph_name: str) -> bool:
        return graph_name in self.graph_defs

    def refresh_graph(self, graph_name: str) -> BuildResult:
        return self.build_graph(graph_name)

    def compact_graph(self, graph_name: str) -> bool:
        return True

    def get_stats(self, graph_name: str) -> GraphStats:
        return GraphStats(
            active_nodes=len(self.nodes.get(graph_name, {})),
            active_edges=len(self.edges.get(graph_name, [])),
        )

    def get_wal_status(self, graph_name: str) -> dict:
        return {"total_entries": 0, "pending_entries": 0, "has_pending": False}

    def purge_wal(self, graph_name: str) -> int:
        return 0

    def neighbors(self, graph_name: str, ext_id: str, direction: str = "out",
                  edge_type: str = "", limit: int = 100) -> list[str]:
        found = []
        for edge in self.edges.get(graph_name, []):
            if edge_type and edge["type"] != edge_type:
                continue
            if direction in ("out", "both") and edge["src"] == ext_id:
                found.append(edge["dst"])
            if direction in ("in", "both") and edge["dst"] == ext_id:
                found.append(edge["src"])
            if len(found) >= limit:
                break
        return found

    def has_edge(self, graph_name: str, src: str, dst: str, edge_type: str = "") -> bool:
        return any(
            edge["src"] == src
            and edge["dst"] == dst
            and (not edge_type or edge["type"] == edge_type)
            for edge in self.edges.get(graph_name, [])
        )

    def on_document_inserted(self, collection: str, bson: str) -> None:
        return None

    def on_document_updated(self, collection: str, old_bson: str, new_bson: str) -> None:
        return None

    def on_document_deleted(self, collection: str, bson: str) -> None:
        return None

    def run_mutual_friends(self, graph_name: str, params: list[str]) -> AlgoResult:
        user1, user2 = params[0], params[1]
        edge_type = params[2] if len(params) > 2 else ""
        mutual = sorted(
            set(self.neighbors(graph_name, user1, "out", edge_type))
            & set(self.neighbors(graph_name, user2, "out", edge_type))
        )
        return algo({"mutual_friends": mutual, "count": len(mutual)})

    def run_most_connected(self, graph_name: str, params: list[str]) -> AlgoResult:
        limit = int(params[0]) if params else 10
        metric = params[1] if len(params) > 1 and params[1] else "out"
        node_type = params[2] if len(params) > 2 else ""
        rows = []
        for node_id, node in self.nodes[graph_name].items():
            if node_type and node["type"] != node_type:
                continue
            out_degree = len(self.neighbors(graph_name, node_id, "out", "", 10_000))
            in_degree = len(self.neighbors(graph_name, node_id, "in", "", 10_000))
            score = out_degree if metric == "out" else in_degree
            rows.append({"id": node_id, "out": out_degree, "in": in_degree, "score": score})
        rows.sort(key=lambda item: (-item["score"], item["id"]))
        return algo({"metric": metric, "limit": limit, "results": rows[:limit]})

    def run_network_stats(self, graph_name: str, params: list[str]) -> AlgoResult:
        node_types = defaultdict(int)
        edge_types = defaultdict(int)
        for node in self.nodes[graph_name].values():
            node_types[node["type"]] += 1
        for edge in self.edges[graph_name]:
            edge_types[edge["type"]] += 1
        stats = self.get_stats(graph_name)
        return algo({
            "mode": params[0] if params else "basic",
            "basic": {"active_nodes": stats.active_nodes, "active_edges": stats.active_edges},
            "node_types": dict(node_types),
            "edge_types": dict(edge_types),
        })

    def run_connected_components(self, graph_name: str, params: list[str]) -> AlgoResult:
        comps = connected_components(self.nodes[graph_name], self.edges[graph_name])
        return algo({
            "total_components": len(comps),
            "total_nodes": len(self.nodes[graph_name]),
            "largest_component_size": max(map(len, comps), default=0),
            "components": [{"size": len(comp), "members": sorted(comp)} for comp in comps],
        })

    def run_community_detection(self, graph_name: str, params: list[str]) -> AlgoResult:
        comps = connected_components(self.nodes[graph_name], self.edges[graph_name])
        communities = [{"id": idx + 1, "size": len(comp), "members": sorted(comp)}
                       for idx, comp in enumerate(comps)]
        return algo({
            "algorithm": "label_propagation",
            "total_communities": len(communities),
            "total_nodes_assigned": sum(item["size"] for item in communities),
            "summary": {"largest_community_size": max((item["size"] for item in communities), default=0)},
            "communities": communities,
        })

    def run_all_distances(self, graph_name: str, params: list[str]) -> AlgoResult:
        source = params[0]
        max_hops = int(params[2]) if len(params) > 2 and params[2] else 10
        distances = bfs_distances(self.edges[graph_name], source, max_hops)
        return algo({
            "mode": "sssp",
            "source": source,
            "max_hops": max_hops,
            "distances": [{"id": node, "distance": dist} for node, dist in sorted(distances.items())],
        })


def algo(payload: dict) -> AlgoResult:
    return AlgoResult(True, result_json=json.dumps(payload), elapsed_ms=0.1)


def connected_components(nodes: dict[str, dict], edges: list[dict]) -> list[set[str]]:
    adjacency: dict[str, set[str]] = {node_id: set() for node_id in nodes}
    for edge in edges:
        adjacency.setdefault(edge["src"], set()).add(edge["dst"])
        adjacency.setdefault(edge["dst"], set()).add(edge["src"])
    seen: set[str] = set()
    comps = []
    for node in sorted(adjacency):
        if node in seen:
            continue
        comp = set()
        queue = deque([node])
        seen.add(node)
        while queue:
            cur = queue.popleft()
            comp.add(cur)
            for nxt in adjacency[cur]:
                if nxt not in seen:
                    seen.add(nxt)
                    queue.append(nxt)
        comps.append(comp)
    return comps


def bfs_distances(edges: list[dict], source: str, max_hops: int) -> dict[str, int]:
    adjacency: dict[str, list[str]] = defaultdict(list)
    for edge in edges:
        adjacency[edge["src"]].append(edge["dst"])
    distances = {source: 0}
    queue = deque([source])
    while queue:
        cur = queue.popleft()
        if distances[cur] >= max_hops:
            continue
        for nxt in adjacency[cur]:
            if nxt not in distances:
                distances[nxt] = distances[cur] + 1
                queue.append(nxt)
    return distances


class NexoraQLScenarioTests(unittest.TestCase):
    def setUp(self):
        self._original_nx = S._nx
        S._nx = lambda: FakeNx
        self.engine = InMemoryDocEngine()
        self.gm = InMemoryGraphManager(self.engine)
        self.executor = Executor(self.engine, self.gm)

    def tearDown(self):
        S._nx = self._original_nx

    def execute(self, query: str) -> list[dict]:
        return self.executor.execute_text(query)

    def test_batch_insert_preserves_nested_json_escapes(self):
        self.execute("CREATE COLLECTION escaped_users;")
        documents = [
            {"_id": "u1", "bio": "first\nsecond"},
            {"_id": "u2", "bio": "O'Connor"},
        ]
        values = []
        for document in documents:
            encoded = json.dumps(document, separators=(",", ":"))
            literal = "'" + encoded.replace("\\", "\\\\").replace("'", "\\'") + "'"
            values.append(f"({literal})")

        [result] = self.execute(
            "INSERT INTO escaped_users BATCH VALUES " + ",".join(values) + ";"
        )

        self.assertTrue(result["success"])
        stored = list(self.engine.collections["escaped_users"].values())
        self.assertEqual(stored, documents)

    def test_full_database_language_flow(self):
        # Query DDL: create a schema-backed collection.
        [create_users] = self.execute("""
            CREATE COLLECTION users (
                _id STRING REQUIRED UNIQUE,
                username STRING REQUIRED UNIQUE,
                age INT64,
                active BOOL,
                status STRING
            ) STRICT;
        """)
        self.assertTrue(create_users["success"])
        self.assertIn("users", self.engine.schemas)
        self.assertTrue(self.engine.schemas["users"].strict)

        # Query DDL: create schemaless collections.
        schemaless = self.execute("""
            CREATE COLLECTION posts;
            CREATE COLLECTION follows;
        """)
        self.assertTrue(all(item["success"] for item in schemaless))

        # Query DML: insert fake documents into collections.
        inserts = self.execute("""
            INSERT INTO users VALUES ('{"_id":"u1","username":"ali","age":28,"active":true,"status":"active","address":{"city":"Tehran"},"tags":["db"],"login_count":0}');
            INSERT INTO users BATCH VALUES
                ('{"_id":"u2","username":"sara","age":25,"active":true,"status":"premium","address":{"city":"Shiraz"},"tags":["graph"],"login_count":2}'),
                ('{"_id":"u3","username":"reza","age":32,"active":true,"status":"active","address":{"city":"Tehran"},"tags":[],"login_count":1}'),
                ('{"_id":"u4","username":"kid","age":16,"active":true,"status":"pending","address":{"city":"Tabriz"},"tags":[],"login_count":0}');
            INSERT INTO posts BATCH VALUES
                ('{"_id":"p1","title":"Hello NexoraDB","author_id":"u1","likes":5,"liked_by":["u2","u3"]}'),
                ('{"_id":"p2","title":"Graph Query","author_id":"u2","likes":1,"liked_by":["u1"]}');
            INSERT INTO follows BATCH VALUES
                ('{"_id":"f1","from_id":"u1","to_id":"u2","since":1700000000}'),
                ('{"_id":"f2","from_id":"u1","to_id":"u3","since":1700001000}'),
                ('{"_id":"f3","from_id":"u2","to_id":"u3","since":1700002000}');
        """)
        self.assertTrue(all(item["success"] for item in inserts))

        # Query DQL: select with projection, nested fields, condition, limit, and skip.
        [adult_users] = self.execute("""
            SELECT _id, username, address.city
            FROM users
            WHERE age >= 18 AND active = true
            LIMIT 10 SKIP 0;
        """)
        self.assertEqual(adult_users["count"], 3)
        self.assertEqual(
            {doc["username"] for doc in adult_users["documents"]},
            {"ali", "sara", "reza"},
        )

        # Query DQL: IN, STARTS WITH, CONTAINS, and COUNT/EXISTS coverage.
        [premium_or_active] = self.execute("""
            SELECT * FROM users
            WHERE status IN ('active', 'premium') AND username STARTS WITH 'a';
        """)
        self.assertEqual([doc["_id"] for doc in premium_or_active["documents"]], ["u1"])

        [contains_graph] = self.execute("""
            SELECT * FROM posts WHERE title CONTAINS 'Graph';
        """)
        self.assertEqual([doc["_id"] for doc in contains_graph["documents"]], ["p2"])

        [tehran_count] = self.execute("COUNT FROM users WHERE address.city = 'Tehran';")
        self.assertEqual(tehran_count["count"], 2)

        [ali_exists] = self.execute("EXISTS IN users WHERE username = 'ali';")
        self.assertTrue(ali_exists["exists"])

        # Query DML: update one document by _id with Set/Inc/Push/Pull/AddToSet/CurrentDate.
        [update_ali] = self.execute("""
            UPDATE users
            SET status='verified', updated_at=NOW()
            INCREMENT login_count BY 1
            PUSH tags='parser'
            PULL tags='db'
            ADD TO SET tags='admin'
            WHERE _id = 'u1';
        """)
        self.assertTrue(update_ali["success"])

        [ali_after_update] = self.execute("SELECT * FROM users WHERE _id = 'u1';")
        ali = ali_after_update["documents"][0]
        self.assertEqual(ali["status"], "verified")
        self.assertEqual(ali["login_count"], 1)
        self.assertEqual(ali["tags"], ["parser", "admin"])
        self.assertEqual(ali["updated_at"], 1782560000000)

        # Query DML: update many documents with a condition.
        [deactivate_minors] = self.execute("""
            UPDATE MANY users SET active=false WHERE age < 18;
        """)
        self.assertEqual(deactivate_minors["updated"], 1)

        # Query DML: delete by condition.
        [delete_minor] = self.execute("DELETE FROM users WHERE _id = 'u4';")
        self.assertEqual(delete_minor["data"], "1")

        [after_delete] = self.execute("EXISTS IN users WHERE _id = 'u4';")
        self.assertFalse(after_delete["exists"])

        # Query DDL: create indexes from query text.
        [idx_username, idx_author] = self.execute("""
            CREATE UNIQUE INDEX idx_users_username ON users (username);
            CREATE INDEX idx_posts_author ON posts (author_id);
        """)
        self.assertTrue(idx_username["success"])
        self.assertTrue(idx_author["success"])
        self.assertIn("idx_users_username", self.engine.indexes["users"])
        self.assertEqual(self.engine.indexes["users"]["idx_users_username"].type, FakeIndexType.Unique)
        [shown_indexes] = self.execute("SHOW INDEXES ON users;")
        self.assertEqual(shown_indexes["indexes"][0]["name"], "idx_users_username")

        # Query DQL: lookup join across collections.
        [joined_posts] = self.execute("""
            SELECT * FROM posts
            LOOKUP JOIN users ON posts.author_id = users._id
            LIMIT 10;
        """)
        self.assertEqual(joined_posts["count"], 2)
        self.assertEqual(joined_posts["documents"][0]["__joined__"][0]["_id"], "u1")

        # Query TCL: commit a transaction and verify the inserted document is visible.
        tx_commit = self.execute("""
            BEGIN TRANSACTION;
            INSERT INTO posts VALUES ('{"_id":"p_tx_commit","title":"Committed","author_id":"u1","likes":0}');
            COMMIT;
        """)
        self.assertEqual(tx_commit[-1]["success"], True)

        [committed_exists] = self.execute("EXISTS IN posts WHERE _id = 'p_tx_commit';")
        self.assertTrue(committed_exists["exists"])

        # Query TCL: rollback a transaction and verify the inserted document is gone.
        tx_rollback = self.execute("""
            BEGIN TRANSACTION;
            INSERT INTO posts VALUES ('{"_id":"p_tx_rollback","title":"Rolled back","author_id":"u1","likes":0}');
            ROLLBACK;
        """)
        self.assertEqual(tx_rollback[-1]["success"], True)

        [rolled_back_exists] = self.execute("EXISTS IN posts WHERE _id = 'p_tx_rollback';")
        self.assertFalse(rolled_back_exists["exists"])

        # Query GDL/GML: create a graph from document collections and build it.
        graph_setup = self.execute("""
            CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
            MAP NODE User FROM users KEY _id PROPERTIES username, age;
            MAP EDGE FOLLOWS FROM follows
                SOURCE from_id AS User
                TARGET to_id AS User
                DIRECTED PROPERTIES since;
            BUILD GRAPH social;
        """)
        self.assertTrue(graph_setup[-1]["success"])
        self.assertEqual(graph_setup[-1]["nodes_built"], 3)
        self.assertEqual(graph_setup[-1]["edges_built"], 3)

        # Query GQL: traversal over the graph.
        [traverse] = self.execute("""
            TRAVERSE User('u1') OUT FOLLOWS DEPTH 1 LIMIT 10;
        """)
        self.assertEqual(set(traverse["nodes"]), {"u2", "u3"})

        # Query GQL: edge existence check.
        [edge_exists] = self.execute("""
            EDGE EXISTS User('u1') -[FOLLOWS]-> User('u2');
        """)
        self.assertTrue(edge_exists["exists"])

        # Query GQL: get a graph node document through the mapping registry.
        [node] = self.execute("GET NODE User('u1');")
        self.assertEqual(node["document"]["username"], "ali")

        # The admin API creates a fresh Executor for every request. GET NODE
        # must therefore recover persisted mappings from GraphManager.
        fresh_executor = Executor(self.engine, self.gm)
        fresh_node = fresh_executor.execute_text(
            "USE GRAPH social; GET NODE User('u1');"
        )[-1]
        self.assertEqual(fresh_node["document"]["username"], "ali")

        # Query GAL: run the implemented lock algorithm MutualFriends.
        [mutual] = self.execute("""
            RUN LOCK MutualFriends ON social
            WITH user1='u1', user2='u2', edge_type='FOLLOWS';
        """)
        self.assertEqual(mutual["result"]["mutual_friends"], ["u3"])

        # Query GAL: run the implemented lock algorithm MostConnected.
        [most_connected] = self.execute("""
            RUN LOCK MostConnected ON social
            WITH metric='out', node_type='User'
            LIMIT 2;
        """)
        self.assertEqual(most_connected["result"]["results"][0]["id"], "u1")

        # Query GAL: run the implemented lock algorithm NetworkStats.
        [network_stats] = self.execute("""
            RUN LOCK NetworkStats ON social WITH mode='full';
        """)
        self.assertEqual(network_stats["result"]["basic"]["active_nodes"], 3)
        self.assertEqual(network_stats["result"]["edge_types"]["FOLLOWS"], 3)

        # Query GAL: run the implemented job algorithm ConnectedComponents and read result by query.
        cc = self.execute("""
            RUN JOB ConnectedComponents ON social WITH node_type='User';
            JOB RESULT 'job_1';
        """)
        self.assertEqual(cc[-1]["result"]["largest_component_size"], 3)

        # Query GAL: run the implemented job algorithm CommunityDetection and read result by query.
        cd = self.execute("""
            RUN JOB CommunityDetection ON social
            WITH max_iterations=10, min_community_size=2, members=true, node_type='User';
            JOB RESULT 'job_2';
        """)
        self.assertEqual(cd[-1]["result"]["algorithm"], "label_propagation")
        self.assertEqual(cd[-1]["result"]["summary"]["largest_community_size"], 3)

        # Query GAL: run the implemented job algorithm AllDistances and read result by query.
        distances = self.execute("""
            RUN JOB AllDistances ON social
            WITH source='u1', all=true, max_hops=2, node_type='User';
            JOB RESULT 'job_3';
        """)
        by_id = {item["id"]: item["distance"] for item in distances[-1]["result"]["distances"]}
        self.assertEqual(by_id["u2"], 1)
        self.assertEqual(by_id["u3"], 1)


if __name__ == "__main__":
    unittest.main()
