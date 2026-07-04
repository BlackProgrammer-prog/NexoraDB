"""
nexoraql.semantic
─────────────────
دو مسئولیت:

  ۱. Validator — بررسی معنایی AST قبل از اجرا
       (UPDATE بدون WHERE، MAP NODE بدون USE GRAPH، الگوریتم Lock با RUN JOB و ...)

  ۲. Executor — تبدیل AST به فراخوانی‌های nexoradb.so
       (DocEngine + GraphManager)

طراحی:
  - import nexoradb به صورت lazy است تا parse بدون .so هم کار کند
  - Executor یک session است: current graph، transaction فعال، job table
"""

from __future__ import annotations

import json
import time
from typing import Any, Optional

from . import ast_nodes as N
from .errors import (
    NexoraQLSemanticError,
    NexoraQLExecutionError,
    NexoraQLUnsupportedError,
)

# ══════════════════════════════════════════════════════════════
# §1  مشخصات ۱۲ الگوریتم
#     order = ترتیب positional که C++ انتظار دارد
#     kind  = lock | job — RUN LOCK/JOB باید مطابقت داشته باشد
# ══════════════════════════════════════════════════════════════

ALGO_SPECS: dict[str, dict] = {
    # ── LockAlgorithm ──
    "MutualFriends":    {"kind": "lock", "order": ["user1", "user2", "edge_type"]},
    "AreConnected":     {"kind": "lock", "order": ["user1", "user2", "edge_type"]},
    "ShortestPath":     {"kind": "lock", "order": ["from", "to", "edge_type", "max_depth"]},
    "FriendSuggestion": {"kind": "lock", "order": ["user", "depth", "limit"]},
    "MostConnected":    {"kind": "lock", "order": ["limit", "metric", "node_type"]},
    "NetworkStats":     {"kind": "lock", "order": ["mode"]},
    "GetFriends":       {"kind": "lock", "order": ["user", "edge_type", "limit"]},
    "Neighborhood":     {"kind": "lock", "order": ["node", "depth", "limit"]},
    # ── JobAlgorithm ──
    "ConnectedComponents": {"kind": "job", "order": ["node_type"]},
    "AllDistances":        {"kind": "job", "order": ["source", "all", "max_hops", "node_type"]},
    "CommunityDetection":  {"kind": "job", "order": ["max_iterations", "min_community_size", "members", "node_type"]},
    "PageRank":            {"kind": "job", "order": ["iterations", "damping", "top"]},
}

# پارامترهای flag: مقدار True → رشته‌ای که C++ انتظار دارد
_FLAG_PARAMS: dict[str, dict[str, str]] = {
    "AllDistances":       {"all": "all"},
    "CommunityDetection": {"members": "members"},
}


def algo_params_to_positional(algo: str, named: dict[str, Any],
                              limit: Optional[int] = None,
                              top: Optional[int] = None) -> list[str]:
    """WITH k=v → vector<string> positional برای C++.

    مثال:
        MutualFriends  WITH user1='u1', user2='u2'
          → ["u1", "u2"]
        MostConnected  WITH metric='in' LIMIT 20
          → ["20", "in", ""]
        AllDistances   WITH source='u1', all=true, max_hops=4
          → ["u1", "all", "4", ""]
    """
    spec = ALGO_SPECS.get(algo)
    named = dict(named)  # کپی

    # LIMIT / RETURNS TOP به slot مناسب می‌رود
    if limit is not None and "limit" not in named:
        named["limit"] = limit
    if top is not None and "top" not in named:
        named["top"] = top

    flags = _FLAG_PARAMS.get(algo, {})

    def to_str(key: str, val: Any) -> str:
        if key in flags:
            return flags[key] if val in (True, "true", 1, flags[key]) else ""
        if isinstance(val, bool):
            return "true" if val else "false"
        if isinstance(val, list):
            # لیست → CSV (الگوریتم‌های آینده که چند id می‌گیرند)
            return ",".join(str(v) for v in val)
        if val is None:
            return ""
        return str(val)

    if spec is None:
        # الگوریتم ناشناخته — پارامترها به ترتیب داده‌شده pass می‌شوند
        return [to_str(k, v) for k, v in named.items()]

    # ترتیب طبق spec — slot های missing = ""
    result: list[str] = []
    used = set()
    for key in spec["order"]:
        if key in named:
            result.append(to_str(key, named[key]))
            used.add(key)
        else:
            result.append("")
    # پارامترهای اضافه ناشناخته را انتها append می‌کنیم
    for k, v in named.items():
        if k not in used:
            result.append(to_str(k, v))

    # trailing "" ها را حذف کن (C++ params.size() چک می‌کند)
    while result and result[-1] == "":
        result.pop()
    return result


# ══════════════════════════════════════════════════════════════
# §2  Validator
# ══════════════════════════════════════════════════════════════

class Validator:
    """بررسی معنایی قبل از اجرا. state سبک نگه می‌دارد (current graph)."""

    def __init__(self):
        self.current_graph: Optional[str] = None

    def validate(self, stmt) -> None:
        t = type(stmt).__name__
        fn = getattr(self, f"_v_{t}", None)
        if fn:
            fn(stmt)

    # ── قوانین ──

    def _v_Update(self, s: N.Update):
        if s.where is None:
            raise NexoraQLSemanticError(
                "UPDATE requires a WHERE clause (use WHERE true to update all)")
        if not s.ops:
            raise NexoraQLSemanticError("UPDATE has no operations")

    def _v_Delete(self, s: N.Delete):
        if s.where is None:
            raise NexoraQLSemanticError(
                "DELETE requires a WHERE clause (use WHERE true to delete all)")

    def _v_UseGraph(self, s: N.UseGraph):
        self.current_graph = s.name

    def _v_CreateGraph(self, s: N.CreateGraph):
        # CREATE GRAPH گراف فعلی را هم set می‌کند (راحتی کاربر)
        self.current_graph = s.name

    def _v_MapNode(self, s: N.MapNode):
        if s.graph is None:
            if self.current_graph is None:
                raise NexoraQLSemanticError(
                    "MAP NODE needs a current graph — run USE GRAPH <name> first")
            s.graph = self.current_graph

    def _v_MapEdge(self, s: N.MapEdge):
        if s.graph is None:
            if self.current_graph is None:
                raise NexoraQLSemanticError(
                    "MAP EDGE needs a current graph — run USE GRAPH <name> first")
            s.graph = self.current_graph
        # UNWIND: باید source یا target به alias اشاره کند
        if s.unwind_path is not None:
            if s.source_path != s.unwind_alias and s.target_path != s.unwind_alias:
                raise NexoraQLSemanticError(
                    f"UNWIND alias '{s.unwind_alias}' is not used as SOURCE or TARGET")

    def _v_Traverse(self, s: N.Traverse):
        if s.graph is None:
            if self.current_graph is None:
                raise NexoraQLSemanticError(
                    "TRAVERSE needs a current graph — run USE GRAPH <name> first")
            s.graph = self.current_graph
        if s.depth < 1:
            raise NexoraQLSemanticError("DEPTH must be >= 1")

    def _v_GetNode(self, s: N.GetNode):
        if s.graph is None:
            if self.current_graph is None:
                raise NexoraQLSemanticError(
                    "GET NODE needs a current graph — run USE GRAPH <name> first")
            s.graph = self.current_graph

    def _v_EdgeExists(self, s: N.EdgeExists):
        if s.graph is None:
            if self.current_graph is None:
                raise NexoraQLSemanticError(
                    "EDGE EXISTS needs a current graph — run USE GRAPH <name> first")
            s.graph = self.current_graph

    def _v_RunLock(self, s: N.RunLock):
        spec = ALGO_SPECS.get(s.algo)
        if spec and spec["kind"] != "lock":
            raise NexoraQLSemanticError(
                f"'{s.algo}' is a JOB algorithm — use RUN JOB instead of RUN LOCK")

    def _v_RunJob(self, s: N.RunJob):
        spec = ALGO_SPECS.get(s.algo)
        if spec and spec["kind"] != "job":
            raise NexoraQLSemanticError(
                f"'{s.algo}' is a LOCK algorithm — use RUN LOCK instead of RUN JOB")


# ══════════════════════════════════════════════════════════════
# §3  Condition / UpdateSpec builders (AST → nexoradb objects)
# ══════════════════════════════════════════════════════════════

def _nx():
    """lazy import — parse بدون .so هم کار می‌کند."""
    import nexoradb
    return nexoradb


_OP_MAP = {
    "EQ": "EQ", "NEQ": "NEQ",
    "GT": "GT", "GTE": "GTE",
    "LT": "LT", "LTE": "LTE",
    "REGEX": "REGEX", "STARTS": "STARTS", "CONTAINS": "CONTAINS",
}


def _value_type(nx, val) -> tuple[str, Any]:
    """مقدار Python → (رشته برای C++, ValueType)"""
    if isinstance(val, bool):
        return ("1" if val else "0", nx.ValueType.Bool)
    if isinstance(val, int):
        return (str(val), nx.ValueType.Int64)
    if isinstance(val, float):
        return (str(val), nx.ValueType.Float64)
    if val is None:
        return ("", nx.ValueType.Null)
    return (str(val), nx.ValueType.String)


def build_condition(node: N.ConditionNode):
    """درخت Condition AST → nexoradb.Condition"""
    nx = _nx()

    if node is None or isinstance(node, N.TrueCond):
        return nx.Condition()  # match-all

    if isinstance(node, N.Cmp):
        raw, vt = _value_type(nx, node.value)
        op = getattr(nx.Op, _OP_MAP[node.op])
        return nx.Condition.leaf(node.field, op, raw, vt)

    if isinstance(node, N.InCmp):
        vals = [str(v) for v in node.values]
        return nx.Condition.in_(node.field, vals, node.negate)

    if isinstance(node, N.ExistsCmp):
        return nx.Condition.leaf(
            node.field, nx.Op.EXISTS,
            "1" if node.positive else "0", nx.ValueType.Bool)

    if isinstance(node, N.And):
        return nx.Condition.and_([build_condition(s) for s in node.subs])

    if isinstance(node, N.Or):
        return nx.Condition.or_([build_condition(s) for s in node.subs])

    if isinstance(node, N.Not):
        return nx.Condition.nor_([build_condition(node.sub)])

    raise NexoraQLSemanticError(f"Unknown condition node: {type(node).__name__}")


def _update_value_type(nx, val):
    if isinstance(val, bool):
        return ("true" if val else "false", nx.UpdateValueType.Bool)
    if isinstance(val, int):
        return (str(val), nx.UpdateValueType.Int64)
    if isinstance(val, float):
        return (str(val), nx.UpdateValueType.Float64)
    if val is None:
        return ("", nx.UpdateValueType.Null)
    return (str(val), nx.UpdateValueType.String)


def build_update_spec(ops: list[N.UpdateOpItem]):
    """لیست UpdateOpItem → nexoradb.UpdateSpec"""
    nx = _nx()
    spec = nx.UpdateSpec()

    for op in ops:
        if op.op == "set":
            raw, vt = _update_value_type(nx, op.value)
            spec.set(op.field, raw, vt)
        elif op.op == "set_now":
            spec.touch_date(op.field)
        elif op.op == "inc":
            spec.inc(op.field, str(op.value), nx.UpdateValueType.Int64
                     if isinstance(op.value, int) else nx.UpdateValueType.Float64)
        elif op.op == "unset":
            spec.unset(op.field)
        elif op.op == "push":
            raw, vt = _update_value_type(nx, op.value)
            spec.push(op.field, raw, vt)
        elif op.op == "pull":
            raw, vt = _update_value_type(nx, op.value)
            spec.pull(op.field, raw, vt)
        elif op.op == "add_to_set":
            # MVP: binding فقط push دارد — add_to_set = push
            # (تکراری بودن در سطح C++ Evaluator با AddToSet چک می‌شود
            #  اگر binding آن اضافه شود اینجا عوض کنید)
            raw, vt = _update_value_type(nx, op.value)
            spec.push(op.field, raw, vt)
        elif op.op == "mul":
            raise NexoraQLUnsupportedError(
                "MULTIPLY not exposed in pybind MVP — add 'mul' to UpdateSpec binding")
        elif op.op in ("min", "max"):
            raise NexoraQLUnsupportedError(
                f"SET {op.op.upper()} not exposed in pybind MVP")
        else:
            raise NexoraQLSemanticError(f"Unknown update op: {op.op}")

    return spec


def _extract_id_eq(node: N.ConditionNode) -> Optional[str]:
    """اگر شرط دقیقاً «_id = X» باشد، X را برمی‌گرداند (برای مسیر O(1))."""
    if isinstance(node, N.Cmp) and node.field == "_id" and node.op == "EQ":
        return str(node.value)
    return None


# ══════════════════════════════════════════════════════════════
# §4  Executor
# ══════════════════════════════════════════════════════════════

class Executor:
    """اجرای AST statements روی DocEngine + GraphManager.

    یک session با state:
      - current_graph (USE GRAPH)
      - transaction فعال
      - job table (RUN JOB → JOB STATUS/RESULT)
      - node mapping registry (برای GET NODE)
      - snapshot registry (SNAPSHOT ... INTO)

    استفاده:
        import nexoradb
        engine = nexoradb.DocEngine("/var/data/db")
        gm     = nexoradb.GraphManager(engine, "./graph_data")
        gm.startup()

        ex = Executor(engine, gm)
        result = ex.execute_text("SELECT * FROM users WHERE age > 18;")
    """

    def __init__(self, engine, graph_manager=None):
        self.engine = engine
        self.gm = graph_manager
        self.validator = Validator()

        self._tx = None                          # transaction فعال
        self._jobs: dict[str, Any] = {}          # job_id → JobHandle
        self._job_meta: dict[str, dict] = {}     # job_id → {algo, graph, submitted}
        self._job_seq = 0
        self._snapshots: dict[str, Any] = {}     # name → StaticGraph
        # graph → {node_type: (collection, key_path)} — برای GET NODE
        self._node_maps: dict[str, dict[str, tuple[str, str]]] = {}
        # runner های Python-side برای الگوریتم‌ها (fallback)
        self._algo_runners: dict[str, Any] = {}

    # ── public API ────────────────────────────────────────────

    def register_algorithm(self, name: str, fn) -> None:
        """ثبت اجرای Python-side یک الگوریتم.
        fn(graph_manager, graph_name, params: list[str]) -> dict
        وقتی binding مستقیم C++ (run_lock/submit_job با string) موجود نیست."""
        self._algo_runners[name] = fn

    def execute_text(self, text: str) -> list[dict]:
        """parse + validate + execute یک متن کامل NexoraQL."""
        from .parser import parse
        results = []
        for stmt in parse(text):
            results.append(self.execute(stmt))
        return results

    def execute(self, stmt) -> dict:
        """اجرای یک AST statement → dict نتیجه."""
        self.validator.validate(stmt)
        t = type(stmt).__name__
        fn = getattr(self, f"_x_{t}", None)
        if fn is None:
            raise NexoraQLUnsupportedError(f"Statement not supported: {t}")
        try:
            return fn(stmt)
        except (NexoraQLSemanticError, NexoraQLExecutionError,
                NexoraQLUnsupportedError):
            raise
        except Exception as e:  # noqa: BLE001
            raise NexoraQLExecutionError(f"{t}: {e}") from e

    # ── helpers ───────────────────────────────────────────────

    @staticmethod
    def _res(r) -> dict:
        """DBResult → dict"""
        return {"success": r.success, "data": r.data, "error": r.error_msg}

    def _require_gm(self):
        if self.gm is None:
            raise NexoraQLExecutionError(
                "GraphManager not attached to this session")
        return self.gm

    # ══════════════════════════════════════════════════════════
    # DDL — Collections
    # ══════════════════════════════════════════════════════════

    def _x_CreateCollection(self, s: N.CreateCollection) -> dict:
        nx = _nx()
        if not s.fields:
            return self._res(self.engine.create_collection(s.name))

        schema = nx.SchemaDefinition()
        schema.strict = s.strict
        type_map = {
            "String": nx.FieldType.String, "Int32": nx.FieldType.Int32,
            "Int64": nx.FieldType.Int64, "Float64": nx.FieldType.Float64,
            "Bool": nx.FieldType.Bool, "Array": nx.FieldType.Array,
            "Object": nx.FieldType.Object, "Binary": nx.FieldType.Binary,
        }
        py_fields = []
        for f in s.fields:
            sf = nx.SchemaField()
            sf.name = f.name
            sf.type = type_map.get(f.dtype, nx.FieldType.String)
            sf.required = f.required
            sf.unique = f.unique
            py_fields.append(sf)
        # bind_vector: assign یکجا (append روی کپی کار می‌کند)
        try:
            schema.fields = nx.SchemaFieldList(py_fields)
        except (AttributeError, TypeError):
            schema.fields = py_fields
        return self._res(self.engine.create_collection(s.name, schema))

    def _x_DropCollection(self, s: N.DropCollection) -> dict:
        return self._res(self.engine.drop_collection(s.name))

    def _x_AlterCollection(self, s: N.AlterCollection) -> dict:
        nx = _nx()
        schema = nx.SchemaDefinition()
        type_map = {
            "String": nx.FieldType.String, "Int32": nx.FieldType.Int32,
            "Int64": nx.FieldType.Int64, "Float64": nx.FieldType.Float64,
            "Bool": nx.FieldType.Bool, "Array": nx.FieldType.Array,
            "Object": nx.FieldType.Object, "Binary": nx.FieldType.Binary,
        }
        py_fields = []
        for f in s.fields:
            sf = nx.SchemaField()
            sf.name = f.name
            sf.type = type_map.get(f.dtype, nx.FieldType.String)
            sf.required = f.required
            sf.unique = f.unique
            py_fields.append(sf)
        try:
            schema.fields = nx.SchemaFieldList(py_fields)
        except (AttributeError, TypeError):
            schema.fields = py_fields
        return self._res(self.engine.set_schema(s.name, schema))

    def _x_ShowCollections(self, s) -> dict:
        return {"success": True, "collections": self.engine.list_collections()}

    def _x_CollectionExists(self, s: N.CollectionExists) -> dict:
        return {"success": True, "exists": self.engine.collection_exists(s.name)}

    def _x_DescribeCollection(self, s: N.DescribeCollection) -> dict:
        out: dict = {"success": True, "collection": s.name}
        out["exists"] = self.engine.collection_exists(s.name)
        out["size"] = self.engine.get_collection_size(s.name)
        schema = self.engine.get_schema(s.name)
        if schema is not None:
            out["schema"] = [
                {"name": f.name, "required": f.required, "unique": f.unique}
                for f in schema.fields
            ]
        fks = self.engine.get_foreign_keys(s.name)
        out["foreign_keys"] = [
            {"name": fk.fk_name, "field": fk.local_field,
             "ref": f"{fk.ref_collection}.{fk.ref_field}"}
            for fk in fks
        ]
        return out

    # ══════════════════════════════════════════════════════════
    # DDL — Index / FK
    # ══════════════════════════════════════════════════════════

    def _x_CreateIndex(self, s: N.CreateIndex) -> dict:
        nx = _nx()
        idx = nx.IndexDefinition()
        idx.index_name = s.index_name
        idx.fields = list(s.fields)
        if s.unique:
            idx.type = nx.IndexType.Unique
        elif len(s.fields) > 1:
            idx.type = nx.IndexType.Compound
        else:
            idx.type = nx.IndexType.SingleField
        return self._res(self.engine.create_index(s.collection, idx))

    def _x_DropIndex(self, s: N.DropIndex) -> dict:
        return self._res(self.engine.drop_index(s.collection, s.index_name))

    def _x_ShowIndexes(self, s: N.ShowIndexes) -> dict:
        raise NexoraQLUnsupportedError(
            "SHOW INDEXES: get_indexes not exposed in pybind MVP")

    def _x_AddForeignKey(self, s: N.AddForeignKey) -> dict:
        nx = _nx()
        fk = nx.ForeignKeyDefinition()
        fk.fk_name = s.fk_name
        fk.local_field = s.local_field
        fk.ref_collection = s.ref_collection
        fk.ref_field = s.ref_field
        return self._res(self.engine.add_foreign_key(s.collection, fk))

    def _x_DropForeignKey(self, s: N.DropForeignKey) -> dict:
        return self._res(self.engine.drop_foreign_key(s.collection, s.fk_name))

    def _x_ShowForeignKeys(self, s: N.ShowForeignKeys) -> dict:
        fks = self.engine.get_foreign_keys(s.collection)
        return {"success": True, "foreign_keys": [
            {"name": fk.fk_name, "field": fk.local_field,
             "ref": f"{fk.ref_collection}.{fk.ref_field}"}
            for fk in fks
        ]}

    # ══════════════════════════════════════════════════════════
    # DML — Insert
    # ══════════════════════════════════════════════════════════

    def _x_Insert(self, s: N.Insert) -> dict:
        # اعتبارسنجی JSON قبل از ارسال به C++
        try:
            json.loads(s.json_doc)
        except json.JSONDecodeError as e:
            raise NexoraQLSemanticError(f"Invalid JSON document: {e}")

        if self._tx is not None:
            r = self.engine.insert_one_tx(self._tx, s.collection, s.json_doc)
        else:
            r = self.engine.insert_one(s.collection, s.json_doc)
            # live-update گراف‌ها
            if r.success and self.gm is not None:
                self.gm.on_document_inserted(s.collection, s.json_doc)
        return self._res(r)

    def _x_InsertBatch(self, s: N.InsertBatch) -> dict:
        for i, doc in enumerate(s.json_docs):
            try:
                json.loads(doc)
            except json.JSONDecodeError as e:
                raise NexoraQLSemanticError(f"Invalid JSON at item {i}: {e}")
        r = self.engine.insert_many(s.collection, list(s.json_docs))
        if r.success and self.gm is not None:
            for doc in s.json_docs:
                self.gm.on_document_inserted(s.collection, doc)
        return self._res(r)

    # ══════════════════════════════════════════════════════════
    # DML — Select / Count / Exists
    # ══════════════════════════════════════════════════════════

    def _x_Select(self, s: N.Select) -> dict:
        # ── LOOKUP JOIN ──
        if s.joins:
            if len(s.joins) > 1:
                raise NexoraQLUnsupportedError(
                    "Multiple LOOKUP JOINs not supported in MVP (engine does one per call)")
            to_col, from_field, to_field = s.joins[0]
            cond = build_condition(s.where)
            jr = self.engine.lookup_join(
                s.collection, from_field, to_col, to_field, cond, s.limit)
            if not jr["success"]:
                return {"success": False, "error": jr["error_msg"]}
            docs = [json.loads(rec) for rec in jr["records"]]
            docs = self._project(docs, s.projection)
            return {"success": True, "count": len(docs), "documents": docs}

        # ── مسیر O(1): WHERE _id = 'x' ──
        doc_id = _extract_id_eq(s.where)
        if doc_id is not None:
            r = self.engine.find_by_id(s.collection, doc_id)
            if not r.success:
                return {"success": True, "count": 0, "documents": []}
            docs = self._project([json.loads(r.data)], s.projection)
            return {"success": True, "count": 1, "documents": docs}

        # ── FindMany عادی ──
        cond = build_condition(s.where)
        r = self.engine.find_many(s.collection, cond, s.limit, s.skip)
        if not r.success:
            return {"success": False, "error": r.error_msg}
        docs = json.loads(r.data) if r.data and r.data.strip() else []
        docs = self._project(docs, s.projection)
        return {"success": True, "count": len(docs), "documents": docs}

    @staticmethod
    def _project(docs: list[dict], projection: Optional[list[str]]) -> list[dict]:
        """اعمال projection در سمت Python (MVP — engine projection ندارد)."""
        if projection is None:
            return docs
        keep = set(projection) | {"_id"}
        out = []
        for d in docs:
            out.append({k: v for k, v in d.items() if k in keep})
        return out

    def _x_Count(self, s: N.Count) -> dict:
        cond = build_condition(s.where)
        r = self.engine.count(s.collection, cond)
        return {"success": r.success,
                "count": int(r.data) if r.success and r.data else 0,
                "error": r.error_msg}

    def _x_ExistsStmt(self, s: N.ExistsStmt) -> dict:
        cond = build_condition(s.where)
        r = self.engine.exists(s.collection, cond)
        return {"success": r.success,
                "exists": (r.data == "true"),
                "error": r.error_msg}

    # ══════════════════════════════════════════════════════════
    # DML — Update / Delete
    # ══════════════════════════════════════════════════════════

    def _x_Update(self, s: N.Update) -> dict:
        spec = build_update_spec(s.ops)

        # مسیر O(1): WHERE _id = 'x' (و MANY نبود)
        doc_id = _extract_id_eq(s.where)
        if doc_id is not None and not s.many:
            old = self.engine.find_by_id(s.collection, doc_id)
            r = self.engine.update_by_id(s.collection, doc_id, spec)
            if r.success and old.success and self.gm is not None:
                new = self.engine.find_by_id(s.collection, doc_id)
                if new.success:
                    self.gm.on_document_updated(s.collection, old.data, new.data)
            return self._res(r)

        cond = build_condition(s.where)
        r = self.engine.update_many(s.collection, cond, spec)
        return {"success": r.success,
                "updated": int(r.data) if r.success and r.data else 0,
                "error": r.error_msg}

    def _x_Delete(self, s: N.Delete) -> dict:
        doc_id = _extract_id_eq(s.where)
        if doc_id is not None:
            old = self.engine.find_by_id(s.collection, doc_id)
            r = self.engine.delete_by_id(s.collection, doc_id)
            if r.success and old.success and self.gm is not None:
                self.gm.on_document_deleted(s.collection, old.data)
            return self._res(r)

        cond = build_condition(s.where)
        r = self.engine.delete_many(s.collection, cond)
        return {"success": r.success,
                "deleted": int(r.data) if r.success and r.data else 0,
                "error": r.error_msg}

    # ══════════════════════════════════════════════════════════
    # TCL
    # ══════════════════════════════════════════════════════════

    def _x_BeginTx(self, s) -> dict:
        if self._tx is not None:
            raise NexoraQLSemanticError("Transaction already active — COMMIT or ROLLBACK first")
        tx = self.engine.begin_transaction()
        if tx is None or not tx.is_valid():
            raise NexoraQLExecutionError("Failed to begin transaction")
        self._tx = tx
        return {"success": True, "transaction": "started"}

    def _x_CommitTx(self, s) -> dict:
        if self._tx is None:
            raise NexoraQLSemanticError("No active transaction")
        r = self.engine.commit_transaction(self._tx)
        self._tx = None
        return self._res(r)

    def _x_RollbackTx(self, s) -> dict:
        if self._tx is None:
            raise NexoraQLSemanticError("No active transaction")
        r = self.engine.rollback_transaction(self._tx)
        self._tx = None
        return self._res(r)

    # ══════════════════════════════════════════════════════════
    # GDL — Graph Definition
    # ══════════════════════════════════════════════════════════

    def _x_CreateGraph(self, s: N.CreateGraph) -> dict:
        nx = _nx()
        gm = self._require_gm()

        gdef = nx.GraphDefinition()
        gdef.name = s.name
        gdef.mode = (nx.GraphMode.Live if s.mode == "live"
                     else nx.GraphMode.Static)
        gdef.directed = s.directed
        gdef.heterogeneous = s.heterogeneous
        gdef.auto_build_on_startup = True

        ok = gm.create_graph(gdef)
        self._node_maps.setdefault(s.name, {})
        return {"success": ok,
                "graph": s.name, "mode": s.mode,
                "error": "" if ok else f"Graph '{s.name}' already exists or failed"}

    def _x_UseGraph(self, s: N.UseGraph) -> dict:
        # validator قبلاً current_graph را set کرده
        return {"success": True, "current_graph": s.name}

    def _x_ShowGraphs(self, s) -> dict:
        gm = self._require_gm()
        return {"success": True, "graphs": gm.list_graphs()}

    def _x_DescribeGraph(self, s: N.DescribeGraph) -> dict:
        gm = self._require_gm()
        st = gm.get_stats(s.name)
        return {"success": True, "graph": s.name,
                "ready": gm.is_ready(s.name),
                "active_nodes": st.active_nodes,
                "active_edges": st.active_edges,
                "version": st.version,
                "node_mappings": self._node_maps.get(s.name, {})}

    def _x_DropGraph(self, s: N.DropGraph) -> dict:
        gm = self._require_gm()
        ok = gm.drop_graph(s.name)
        self._node_maps.pop(s.name, None)
        return {"success": ok, "dropped": s.name}

    def _x_MapNode(self, s: N.MapNode) -> dict:
        nx = _nx()
        gm = self._require_gm()

        nm = nx.NodeMappingDef()
        nm.node_type = s.node_type
        nm.collection = s.collection
        nm.key_path = s.key_path
        nm.properties = list(s.properties)
        # WHERE فیلتر — MVP فقط ثبت (C++ filter_expr در MVP اعمال نمی‌شود)
        if s.where is not None and not isinstance(s.where, N.TrueCond):
            nm.filter_expr = _cond_to_str(s.where)

        ok = gm.add_node_mapping(s.graph, nm)
        # ثبت برای GET NODE
        self._node_maps.setdefault(s.graph, {})[s.node_type] = (
            s.collection, s.key_path)
        return {"success": ok, "graph": s.graph,
                "node_type": s.node_type, "collection": s.collection}

    def _x_MapEdge(self, s: N.MapEdge) -> dict:
        nx = _nx()
        gm = self._require_gm()

        em = nx.EdgeMappingDef()
        em.edge_type = s.edge_type
        em.collection = s.collection
        em.source_path = s.source_path
        em.source_node_type = s.source_node_type
        em.target_path = s.target_path
        em.target_node_type = s.target_node_type
        em.directed = s.directed
        em.properties = list(s.properties)

        if s.unwind_path is not None:
            uw = nx.UnwindConfig()
            uw.array_path = s.unwind_path
            uw.alias = s.unwind_alias
            em.unwind = uw

        ok = gm.add_edge_mapping(s.graph, em)
        return {"success": ok, "graph": s.graph,
                "edge_type": s.edge_type,
                "method": ("unwind" if s.unwind_path else "field")}

    # ══════════════════════════════════════════════════════════
    # GML — Build / Maintain
    # ══════════════════════════════════════════════════════════

    def _x_BuildGraph(self, s: N.BuildGraph) -> dict:
        gm = self._require_gm()
        br = gm.build_graph(s.name)
        return {"success": br.success,
                "nodes_built": br.nodes_built,
                "edges_built": br.edges_built,
                "elapsed_ms": br.elapsed_ms,
                "error": br.error_msg}

    def _x_RenderGraph(self, s: N.RenderGraph) -> dict:
        gm = self._require_gm()
        return {"success": gm.render_graph(s.name), "graph": s.name}

    def _x_RefreshGraph(self, s: N.RefreshGraph) -> dict:
        gm = self._require_gm()
        if s.every_hours is not None:
            raise NexoraQLUnsupportedError(
                "REFRESH ... EVERY needs a scheduler — run REFRESH manually or use cron/FastAPI")
        br = gm.refresh_graph(s.name)
        return {"success": br.success,
                "nodes_built": br.nodes_built,
                "edges_built": br.edges_built,
                "error": br.error_msg}

    def _x_CompactGraph(self, s: N.CompactGraph) -> dict:
        gm = self._require_gm()
        return {"success": gm.compact_graph(s.name), "graph": s.name}

    def _x_GraphWalStatus(self, s: N.GraphWalStatus) -> dict:
        gm = self._require_gm()
        ws = gm.get_wal_status(s.name)
        return {"success": True, "graph": s.name, **ws}

    def _x_PurgeWal(self, s: N.PurgeWal) -> dict:
        gm = self._require_gm()
        purged = gm.purge_wal(s.name)
        return {"success": True, "graph": s.name, "purged": purged}

    def _x_ReplayWal(self, s: N.ReplayWal) -> dict:
        raise NexoraQLUnsupportedError(
            "REPLAY GRAPH WAL happens automatically at startup — manual replay not exposed")

    def _x_GraphStatus(self, s: N.GraphStatus) -> dict:
        gm = self._require_gm()
        st = gm.get_stats(s.name)
        return {"success": True, "graph": s.name,
                "ready": gm.is_ready(s.name),
                "state": "Clean" if gm.is_ready(s.name) else "NotBuilt",
                "nodes": st.active_nodes, "edges": st.active_edges,
                "version": st.version}

    def _x_GraphStats(self, s: N.GraphStats) -> dict:
        gm = self._require_gm()
        st = gm.get_stats(s.name)
        return {"success": True, "graph": s.name,
                "active_nodes": st.active_nodes,
                "active_edges": st.active_edges,
                "deleted_nodes": st.deleted_nodes,
                "deleted_edges": st.deleted_edges,
                "heavy_nodes": st.heavy_nodes,
                "version": st.version}

    def _x_SnapshotGraph(self, s: N.SnapshotGraph) -> dict:
        gm = self._require_gm()
        snap = gm.create_snapshot(s.name)
        if snap is None:
            raise NexoraQLExecutionError(f"Snapshot failed for graph '{s.name}'")
        self._snapshots[s.into] = snap
        st = snap.stats()
        return {"success": True, "snapshot": s.into,
                "nodes": st.node_count, "edges": st.edge_count,
                "version": st.version}

    # ══════════════════════════════════════════════════════════
    # GQL — Traversal
    # ══════════════════════════════════════════════════════════

    def _x_Traverse(self, s: N.Traverse) -> dict:
        gm = self._require_gm()
        edge_type = s.edge_type or ""

        if s.depth == 1:
            ids = gm.neighbors(s.graph, s.node_id, s.direction,
                               edge_type, s.limit)
            return {"success": True, "source": s.node_id,
                    "depth": 1, "count": len(ids), "nodes": ids}

        # depth > 1 → BFS با neighbors تکراری (سمت Python)
        visited = {s.node_id}
        frontier = [s.node_id]
        found: list[dict] = []
        for d in range(1, s.depth + 1):
            next_frontier = []
            for nid in frontier:
                for nbr in gm.neighbors(s.graph, nid, s.direction,
                                        edge_type, s.limit):
                    if nbr in visited:
                        continue
                    visited.add(nbr)
                    found.append({"id": nbr, "distance": d})
                    next_frontier.append(nbr)
                    if len(found) >= s.limit:
                        break
                if len(found) >= s.limit:
                    break
            frontier = next_frontier
            if not frontier or len(found) >= s.limit:
                break

        return {"success": True, "source": s.node_id,
                "depth": s.depth, "count": len(found), "nodes": found}

    def _x_GetNode(self, s: N.GetNode) -> dict:
        # از node mapping registry: node_type → (collection, key)
        mapping = self._node_maps.get(s.graph, {}).get(s.node_type)
        if mapping is None:
            raise NexoraQLSemanticError(
                f"Node type '{s.node_type}' not mapped in graph '{s.graph}' "
                f"(run MAP NODE first, in this session)")
        collection, _key = mapping
        r = self.engine.find_by_id(collection, s.node_id)
        if not r.success:
            return {"success": False, "error": r.error_msg}
        return {"success": True, "node_type": s.node_type,
                "document": json.loads(r.data)}

    def _x_EdgeExists(self, s: N.EdgeExists) -> dict:
        gm = self._require_gm()
        exists = gm.has_edge(s.graph, s.src_id, s.dst_id, s.edge_type)
        return {"success": True, "exists": exists,
                "src": s.src_id, "dst": s.dst_id, "edge_type": s.edge_type}

    # ══════════════════════════════════════════════════════════
    # GAL — Algorithms
    # ══════════════════════════════════════════════════════════

    def _x_RunLock(self, s: N.RunLock) -> dict:
        gm = self._require_gm()
        params = algo_params_to_positional(s.algo, s.params, limit=s.limit)

        # مسیر ۱: binding مستقیم C++ (اگر GraphManager.run_lock داشته باشد)
        if hasattr(gm, "run_lock"):
            try:
                r = gm.run_lock(s.graph, s.algo, params)
                if getattr(r, "success", False):
                    return {"success": True, "algo": s.algo,
                            "elapsed_ms": r.elapsed_ms,
                            "result": _try_json(r.result_json)}
                # binding MVP خطای «not supported» می‌دهد → fallback
            except Exception:  # noqa: BLE001
                pass

        # مسیر ۲: runner ثبت‌شده Python
        runner = self._algo_runners.get(s.algo)
        if runner is not None:
            out = runner(gm, s.graph, params)
            return {"success": True, "algo": s.algo, "result": out}

        raise NexoraQLUnsupportedError(
            f"Algorithm '{s.algo}' has no dispatcher. Either expose "
            f"GraphManager.run_lock(graph, algo_name, params) in the pybind "
            f"binding (via AlgorithmRegistry) or register a Python runner: "
            f"executor.register_algorithm('{s.algo}', fn)")

    def _x_RunJob(self, s: N.RunJob) -> dict:
        gm = self._require_gm()
        params = algo_params_to_positional(s.algo, s.params, top=s.returns_top)

        # مسیر ۱: binding مستقیم
        if hasattr(gm, "submit_job"):
            try:
                handle = gm.submit_job(s.graph, s.algo, params)
                job_id = getattr(handle, "job_id", None) or self._next_job_id()
                self._jobs[job_id] = handle
                self._job_meta[job_id] = {
                    "algo": s.algo, "graph": s.graph,
                    "submitted": time.time()}
                return {"success": True, "job_id": job_id,
                        "algo": s.algo, "status": "submitted"}
            except Exception:  # noqa: BLE001
                pass

        # مسیر ۲: runner ثبت‌شده Python — سنکرون اجرا می‌شود
        runner = self._algo_runners.get(s.algo)
        if runner is not None:
            job_id = self._next_job_id()
            out = runner(gm, s.graph, params)
            self._jobs[job_id] = _DoneJob(out)
            self._job_meta[job_id] = {
                "algo": s.algo, "graph": s.graph, "submitted": time.time()}
            return {"success": True, "job_id": job_id,
                    "algo": s.algo, "status": "done"}

        raise NexoraQLUnsupportedError(
            f"Algorithm '{s.algo}' has no dispatcher. Expose "
            f"GraphManager.submit_job in pybind or register a Python runner.")

    def _next_job_id(self) -> str:
        self._job_seq += 1
        return f"job_{self._job_seq}"

    def _x_JobStatus(self, s: N.JobStatus) -> dict:
        handle = self._jobs.get(s.job_id)
        if handle is None:
            return {"success": False, "error": f"Job not found: {s.job_id}"}
        meta = self._job_meta.get(s.job_id, {})
        done = handle.is_done() if hasattr(handle, "is_done") else True
        return {"success": True, "job_id": s.job_id,
                "status": "done" if done else "running", **meta}

    def _x_JobResult(self, s: N.JobResult) -> dict:
        handle = self._jobs.get(s.job_id)
        if handle is None:
            return {"success": False, "error": f"Job not found: {s.job_id}"}
        r = handle.result()  # blocking
        if isinstance(r, dict):
            return {"success": True, "job_id": s.job_id, "result": r}
        return {"success": getattr(r, "success", True),
                "job_id": s.job_id,
                "elapsed_ms": getattr(r, "elapsed_ms", 0.0),
                "result": _try_json(getattr(r, "result_json", "")),
                "error": getattr(r, "error_msg", "")}

    def _x_JobCancel(self, s: N.JobCancel) -> dict:
        # std::future قابل cancel نیست — فقط از registry حذف می‌کنیم
        if s.job_id in self._jobs:
            self._jobs.pop(s.job_id)
            self._job_meta.pop(s.job_id, None)
            return {"success": True, "job_id": s.job_id,
                    "note": "removed from registry (C++ future cannot be cancelled)"}
        return {"success": False, "error": f"Job not found: {s.job_id}"}

    def _x_ShowJobs(self, s: N.ShowJobs) -> dict:
        jobs = []
        for jid, handle in self._jobs.items():
            meta = self._job_meta.get(jid, {})
            if s.graph and meta.get("graph") != s.graph:
                continue
            done = handle.is_done() if hasattr(handle, "is_done") else True
            jobs.append({"job_id": jid,
                         "status": "done" if done else "running", **meta})
        return {"success": True, "jobs": jobs}

    # ══════════════════════════════════════════════════════════
    # SYS
    # ══════════════════════════════════════════════════════════

    def _x_SystemStatus(self, s) -> dict:
        out = {"success": True,
               "healthy": self.engine.is_healthy(),
               "collections": len(self.engine.list_collections())}
        if self.gm is not None:
            out["graphs"] = len(self.gm.list_graphs())
        out["transaction_active"] = self._tx is not None
        out["jobs"] = len(self._jobs)
        return out

    def _x_SystemInfo(self, s) -> dict:
        nx = _nx()
        return {"success": True,
                "version": getattr(nx, "__version__", "?"),
                "graph_enabled": getattr(nx, "GRAPH_ENABLED", False),
                "collections": self.engine.list_collections(),
                "graphs": (self.gm.list_graphs() if self.gm else []),
                "algorithms": sorted(ALGO_SPECS.keys())}


# ══════════════════════════════════════════════════════════════
# §5  helpers
# ══════════════════════════════════════════════════════════════

class _DoneJob:
    """job سنکرون Python-side که از قبل تمام شده."""

    def __init__(self, result):
        self._result = result

    def is_done(self) -> bool:
        return True

    def result(self):
        return self._result


def _try_json(s: str):
    """اگر JSON معتبر بود parse کن، وگرنه رشته خام."""
    if not s:
        return None
    try:
        return json.loads(s)
    except (json.JSONDecodeError, TypeError):
        return s


def _cond_to_str(node: N.ConditionNode) -> str:
    """شرط → رشته نمایشی (برای filter_expr در MVP)."""
    if node is None or isinstance(node, N.TrueCond):
        return ""
    if isinstance(node, N.Cmp):
        return f"{node.field}{_op_sym(node.op)}{node.value}"
    if isinstance(node, N.And):
        return " AND ".join(_cond_to_str(x) for x in node.subs)
    if isinstance(node, N.Or):
        return " OR ".join(_cond_to_str(x) for x in node.subs)
    if isinstance(node, N.Not):
        return f"NOT ({_cond_to_str(node.sub)})"
    if isinstance(node, N.InCmp):
        neg = "NOT " if node.negate else ""
        return f"{node.field} {neg}IN ({','.join(map(str, node.values))})"
    if isinstance(node, N.ExistsCmp):
        return f"{node.field} {'EXISTS' if node.positive else 'NOT EXISTS'}"
    return ""


def _op_sym(op: str) -> str:
    return {"EQ": "=", "NEQ": "!=", "GT": ">", "GTE": ">=",
            "LT": "<", "LTE": "<=", "REGEX": " LIKE ",
            "STARTS": " STARTS WITH ", "CONTAINS": " CONTAINS "}.get(op, op)