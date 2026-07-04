"""
nexoraql.ast_nodes
──────────────────
گره‌های AST — خروجی transformer، ورودی semantic/executor.

هر statement یک dataclass است. Condition ها درخت بازگشتی هستند.
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Any, Optional, Union


# ══════════════════════════════════════════════════════════════
# §1  Condition tree (WHERE)
# ══════════════════════════════════════════════════════════════

@dataclass
class Cmp:
    """مقایسه ساده: field OP value
    op ∈ {"EQ","NEQ","GT","GTE","LT","LTE","REGEX","STARTS","CONTAINS"}"""
    field: str
    op: str
    value: Any            # str | int | float | bool | None


@dataclass
class InCmp:
    """field IN (v1, v2, ...) — negate=True برای NOT IN"""
    field: str
    values: list
    negate: bool = False


@dataclass
class ExistsCmp:
    """field EXISTS — positive=False برای NOT EXISTS"""
    field: str
    positive: bool = True


@dataclass
class And:
    subs: list = field(default_factory=list)


@dataclass
class Or:
    subs: list = field(default_factory=list)


@dataclass
class Not:
    sub: Any = None


@dataclass
class TrueCond:
    """WHERE true — match همه"""
    pass


ConditionNode = Union[Cmp, InCmp, ExistsCmp, And, Or, Not, TrueCond, None]


# ══════════════════════════════════════════════════════════════
# §2  DDL — Collections / Index / FK
# ══════════════════════════════════════════════════════════════

@dataclass
class SchemaFieldDef:
    name: str
    dtype: str                 # "String","Int32","Int64","Float64","Bool","Array","Object","Binary"
    required: bool = False
    unique: bool = False
    default: Any = None


@dataclass
class CreateCollection:
    name: str
    fields: list[SchemaFieldDef] = field(default_factory=list)
    strict: bool = False


@dataclass
class DropCollection:
    name: str


@dataclass
class AlterCollection:
    name: str
    fields: list[SchemaFieldDef] = field(default_factory=list)


@dataclass
class ShowCollections:
    pass


@dataclass
class CollectionExists:
    name: str


@dataclass
class DescribeCollection:
    name: str


@dataclass
class CreateIndex:
    index_name: str
    collection: str
    fields: list[str]
    unique: bool = False


@dataclass
class DropIndex:
    index_name: str
    collection: str


@dataclass
class ShowIndexes:
    collection: str


@dataclass
class AddForeignKey:
    fk_name: str
    collection: str
    local_field: str
    ref_collection: str
    ref_field: str


@dataclass
class DropForeignKey:
    fk_name: str
    collection: str


@dataclass
class ShowForeignKeys:
    collection: str


# ══════════════════════════════════════════════════════════════
# §3  DML
# ══════════════════════════════════════════════════════════════

@dataclass
class Insert:
    collection: str
    json_doc: str


@dataclass
class InsertBatch:
    collection: str
    json_docs: list[str]


@dataclass
class Select:
    collection: str
    projection: Optional[list[str]]        # None = *
    joins: list[tuple[str, str, str]]      # (to_col, from_field, to_field)
    where: ConditionNode = None
    limit: int = 0
    skip: int = 0


@dataclass
class Count:
    collection: str
    where: ConditionNode = None


@dataclass
class ExistsStmt:
    collection: str
    where: ConditionNode = None


@dataclass
class UpdateOpItem:
    """یک عملیات update:
    op ∈ {"set","set_now","inc","unset","push","pull","add_to_set","mul","min","max"}"""
    op: str
    field: str
    value: Any = None


@dataclass
class Update:
    collection: str
    ops: list[UpdateOpItem]
    where: ConditionNode
    many: bool = False


@dataclass
class Delete:
    collection: str
    where: ConditionNode


# ══════════════════════════════════════════════════════════════
# §4  TCL
# ══════════════════════════════════════════════════════════════

@dataclass
class BeginTx:
    pass


@dataclass
class CommitTx:
    pass


@dataclass
class RollbackTx:
    pass


# ══════════════════════════════════════════════════════════════
# §5  GDL — Graph Definition
# ══════════════════════════════════════════════════════════════

@dataclass
class CreateGraph:
    name: str
    mode: str = "live"              # "live" | "static"
    directed: bool = True
    heterogeneous: bool = True
    where: ConditionNode = None     # فیلتر کلی (رزرو برای آینده)


@dataclass
class UseGraph:
    name: str


@dataclass
class ShowGraphs:
    pass


@dataclass
class DescribeGraph:
    name: str


@dataclass
class DropGraph:
    name: str


@dataclass
class MapNode:
    node_type: str
    collection: str
    key_path: str
    properties: list[str] = field(default_factory=list)
    where: ConditionNode = None
    graph: Optional[str] = None     # None = گراف فعلی (USE GRAPH)


@dataclass
class MapEdge:
    edge_type: str
    collection: str
    source_path: str
    source_node_type: str
    target_path: str
    target_node_type: str
    directed: bool = True
    properties: list[str] = field(default_factory=list)
    unwind_path: Optional[str] = None
    unwind_alias: Optional[str] = None
    graph: Optional[str] = None


# ══════════════════════════════════════════════════════════════
# §6  GML — Build / Maintain
# ══════════════════════════════════════════════════════════════

@dataclass
class BuildGraph:
    name: str
    option: Optional[str] = None    # "verbose"|"nodes_only"|"edges_only"


@dataclass
class RenderGraph:
    name: str


@dataclass
class RefreshGraph:
    name: str
    every_hours: Optional[int] = None


@dataclass
class CompactGraph:
    name: str


@dataclass
class GraphWalStatus:
    name: str


@dataclass
class PurgeWal:
    name: str


@dataclass
class ReplayWal:
    name: str


@dataclass
class GraphStatus:
    name: str


@dataclass
class GraphStats:
    name: str


@dataclass
class SnapshotGraph:
    name: str
    into: str


# ══════════════════════════════════════════════════════════════
# §7  GQL — Traversal
# ══════════════════════════════════════════════════════════════

@dataclass
class Traverse:
    node_type: str
    node_id: str
    direction: str                  # "out"|"in"|"both"
    edge_type: Optional[str]        # None = همه (*)
    depth: int = 1
    limit: int = 100
    graph: Optional[str] = None


@dataclass
class GetNode:
    node_type: str
    node_id: str
    graph: Optional[str] = None


@dataclass
class EdgeExists:
    src_type: str
    src_id: str
    edge_type: str
    dst_type: str
    dst_id: str
    graph: Optional[str] = None


# ══════════════════════════════════════════════════════════════
# §8  GAL — Algorithms
# ══════════════════════════════════════════════════════════════

@dataclass
class RunLock:
    algo: str
    graph: str
    params: dict[str, Any] = field(default_factory=dict)
    limit: Optional[int] = None


@dataclass
class RunJob:
    algo: str
    graph: str
    params: dict[str, Any] = field(default_factory=dict)
    returns_top: Optional[int] = None


@dataclass
class JobStatus:
    job_id: str


@dataclass
class JobResult:
    job_id: str


@dataclass
class JobCancel:
    job_id: str


@dataclass
class ShowJobs:
    graph: Optional[str] = None


# ══════════════════════════════════════════════════════════════
# §9  SYS
# ══════════════════════════════════════════════════════════════

@dataclass
class SystemStatus:
    pass


@dataclass
class SystemInfo:
    pass