"""
nexoraql.transformer
────────────────────
تبدیل درخت Lark به AST (ast_nodes).

هر متد این کلاس با نام یک rule یا alias در گرامر مطابقت دارد.
Lark به‌طور خودکار children را به متد pass می‌کند.
"""

from __future__ import annotations

import ast

from lark import Token, Transformer

from . import ast_nodes as N


def _tok(x) -> str:
    """Token → str"""
    return str(x)


def _unquote(s: str) -> str:
    """Parse a quoted literal without corrupting nested JSON escapes."""
    if len(s) >= 2 and s[0] in "'\"" and s[-1] == s[0]:
        try:
            value = ast.literal_eval(s)
        except (SyntaxError, ValueError):
            return s[1:-1]
        return value if isinstance(value, str) else str(value)
    return s


class NexoraQLTransformer(Transformer):
    """Lark tree → لیست AST statements"""

    # ══════════════════════════════════════════════════════════
    # ریشه
    # ══════════════════════════════════════════════════════════

    def start(self, stmts):
        return list(stmts)

    # ══════════════════════════════════════════════════════════
    # Values
    # ══════════════════════════════════════════════════════════

    def string(self, ch):
        return _unquote(_tok(ch[0]))

    def number(self, ch):
        s = _tok(ch[0])
        if "." in s or "e" in s.lower():
            return float(s)
        return int(s)

    def vtrue(self, _):  return True
    def vfalse(self, _): return False
    def vnull(self, _):  return None

    def field_path(self, parts):
        return ".".join(_tok(p) for p in parts)

    def value_list(self, vals):
        return list(vals)

    def wlist(self, vals):
        return list(vals)

    # ══════════════════════════════════════════════════════════
    # §1  Collections DDL
    # ══════════════════════════════════════════════════════════

    # dtype aliases
    def t_string(self, _):  return "String"
    def t_int32(self, _):   return "Int32"
    def t_int64(self, _):   return "Int64"
    def t_float64(self, _): return "Float64"
    def t_bool(self, _):    return "Bool"
    def t_array(self, _):   return "Array"
    def t_object(self, _):  return "Object"
    def t_binary(self, _):  return "Binary"

    # flag aliases
    def f_required(self, _): return ("required", True)
    def f_unique(self, _):   return ("unique", True)
    def f_default(self, ch): return ("default", ch[0])

    def field_def(self, ch):
        name = _tok(ch[0])
        dtype = ch[1]
        fd = N.SchemaFieldDef(name=name, dtype=dtype)
        for flag in ch[2:]:
            k, v = flag
            setattr(fd, k, v)
        return fd

    def schema_body(self, fields):
        return list(fields)

    def strict_kw(self, _):
        return "STRICT"

    def create_collection(self, ch):
        name = _tok(ch[0])
        fields, strict = [], False
        for item in ch[1:]:
            if item == "STRICT":
                strict = True
            elif isinstance(item, list):
                fields = item
        return N.CreateCollection(name=name, fields=fields, strict=strict)

    def drop_collection(self, ch):
        return N.DropCollection(name=_tok(ch[0]))

    def alter_collection(self, ch):
        return N.AlterCollection(name=_tok(ch[0]), fields=ch[1])

    def show_collections(self, _):
        return N.ShowCollections()

    def collection_exists(self, ch):
        return N.CollectionExists(name=_tok(ch[0]))

    def describe_collection(self, ch):
        return N.DescribeCollection(name=_tok(ch[0]))

    # ══════════════════════════════════════════════════════════
    # §2  Index / FK
    # ══════════════════════════════════════════════════════════

    def unique_kw(self, _):
        return "UNIQUE"

    def create_index(self, ch):
        unique = False
        idx = 0
        if ch and ch[0] == "UNIQUE":
            unique = True
            idx = 1
        index_name = _tok(ch[idx])
        collection = _tok(ch[idx + 1])
        fields = [_tok(t) for t in ch[idx + 2:]]
        return N.CreateIndex(index_name=index_name, collection=collection,
                             fields=fields, unique=unique)

    def drop_index(self, ch):
        return N.DropIndex(index_name=_tok(ch[0]), collection=_tok(ch[1]))

    def show_indexes(self, ch):
        return N.ShowIndexes(collection=_tok(ch[0]))

    def add_fk(self, ch):
        return N.AddForeignKey(
            fk_name=_tok(ch[0]), collection=_tok(ch[1]),
            local_field=_tok(ch[2]),
            ref_collection=_tok(ch[3]), ref_field=_tok(ch[4]))

    def drop_fk(self, ch):
        return N.DropForeignKey(fk_name=_tok(ch[0]), collection=_tok(ch[1]))

    def show_fks(self, ch):
        return N.ShowForeignKeys(collection=_tok(ch[0]))

    # ══════════════════════════════════════════════════════════
    # §3  Insert
    # ══════════════════════════════════════════════════════════

    def insert_stmt(self, ch):
        return N.Insert(collection=_tok(ch[0]), json_doc=ch[1])

    def batch_item(self, ch):
        return ch[0]

    def insert_batch(self, ch):
        return N.InsertBatch(collection=_tok(ch[0]), json_docs=list(ch[1:]))

    # ══════════════════════════════════════════════════════════
    # §4  Select / Count / Exists
    # ══════════════════════════════════════════════════════════

    def proj_all(self, _):
        return None  # None = *

    def proj_fields(self, fields):
        return list(fields)

    def join_clause(self, ch):
        to_col = _tok(ch[0])
        from_path = ch[1]   # "posts.author_id"
        to_path = ch[2]     # "users._id"
        from_field = from_path.split(".", 1)[1] if "." in from_path else from_path
        to_field = to_path.split(".", 1)[1] if "." in to_path else to_path
        return (to_col, from_field, to_field)

    def limit_clause(self, ch):
        return ("limit", int(_tok(ch[0])))

    def skip_clause(self, ch):
        return ("skip", int(_tok(ch[0])))

    def where_clause(self, ch):
        return ("where", ch[0])

    def select_stmt(self, ch):
        collection_idx = 1
        projection = ch[0]
        collection = _tok(ch[collection_idx])
        joins, where, limit, skip = [], None, 0, 0
        for item in ch[collection_idx + 1:]:
            if isinstance(item, tuple):
                if item[0] == "where":
                    where = item[1]
                elif item[0] == "limit":
                    limit = item[1]
                elif item[0] == "skip":
                    skip = item[1]
                elif len(item) == 3:      # join tuple
                    joins.append(item)
        return N.Select(collection=collection, projection=projection,
                        joins=joins, where=where, limit=limit, skip=skip)

    def count_stmt(self, ch):
        where = None
        for item in ch[1:]:
            if isinstance(item, tuple) and item[0] == "where":
                where = item[1]
        return N.Count(collection=_tok(ch[0]), where=where)

    def exists_stmt(self, ch):
        where = None
        for item in ch[1:]:
            if isinstance(item, tuple) and item[0] == "where":
                where = item[1]
        return N.ExistsStmt(collection=_tok(ch[0]), where=where)

    # ══════════════════════════════════════════════════════════
    # §5  Update
    # ══════════════════════════════════════════════════════════

    def many_kw(self, _):
        return "MANY"

    def assign_value(self, ch):
        return N.UpdateOpItem(op="set", field=ch[0], value=ch[1])

    def assign_now(self, ch):
        return N.UpdateOpItem(op="set_now", field=ch[0])

    def set_op(self, items):
        return list(items)

    def inc_pair(self, ch):
        return N.UpdateOpItem(op="inc", field=ch[0], value=_num(_tok(ch[1])))

    def inc_op(self, pairs):
        return list(pairs)

    def unset_op(self, fields):
        return [N.UpdateOpItem(op="unset", field=f) for f in fields]

    def push_op(self, ch):
        return [N.UpdateOpItem(op="push", field=ch[0], value=ch[1])]

    def pull_op(self, ch):
        return [N.UpdateOpItem(op="pull", field=ch[0], value=ch[1])]

    def addtoset_op(self, ch):
        return [N.UpdateOpItem(op="add_to_set", field=ch[0], value=ch[1])]

    def mul_pair(self, ch):
        return N.UpdateOpItem(op="mul", field=ch[0], value=_num(_tok(ch[1])))

    def mul_op(self, pairs):
        return list(pairs)

    def setmin_op(self, ch):
        return [N.UpdateOpItem(op="min", field=ch[0], value=ch[1])]

    def setmax_op(self, ch):
        return [N.UpdateOpItem(op="max", field=ch[0], value=ch[1])]

    def update_stmt(self, ch):
        many = False
        idx = 0
        if ch and ch[0] == "MANY":
            many = True
            idx = 1
        collection = _tok(ch[idx])
        ops: list[N.UpdateOpItem] = []
        where = None
        for item in ch[idx + 1:]:
            if isinstance(item, tuple) and item[0] == "where":
                where = item[1]
            elif isinstance(item, list):
                ops.extend(item)
        return N.Update(collection=collection, ops=ops, where=where, many=many)

    # ══════════════════════════════════════════════════════════
    # §6  Delete
    # ══════════════════════════════════════════════════════════

    def delete_stmt(self, ch):
        where = None
        for item in ch[1:]:
            if isinstance(item, tuple) and item[0] == "where":
                where = item[1]
        return N.Delete(collection=_tok(ch[0]), where=where)

    # ══════════════════════════════════════════════════════════
    # §7  TCL
    # ══════════════════════════════════════════════════════════

    def begin_tx(self, _):    return N.BeginTx()
    def commit_tx(self, _):   return N.CommitTx()
    def rollback_tx(self, _): return N.RollbackTx()

    # ══════════════════════════════════════════════════════════
    # §8  Condition tree
    # ══════════════════════════════════════════════════════════

    def op_eq(self, _):  return "EQ"
    def op_neq(self, _): return "NEQ"
    def op_gt(self, _):  return "GT"
    def op_gte(self, _): return "GTE"
    def op_lt(self, _):  return "LT"
    def op_lte(self, _): return "LTE"

    def cmp(self, ch):
        return N.Cmp(field=ch[0], op=ch[1], value=ch[2])

    def in_cmp(self, ch):
        return N.InCmp(field=ch[0], values=ch[1], negate=False)

    def nin_cmp(self, ch):
        return N.InCmp(field=ch[0], values=ch[1], negate=True)

    def exists_cmp(self, ch):
        return N.ExistsCmp(field=ch[0], positive=True)

    def nexists_cmp(self, ch):
        return N.ExistsCmp(field=ch[0], positive=False)

    def like_cmp(self, ch):
        return N.Cmp(field=ch[0], op="REGEX", value=ch[1])

    def starts_cmp(self, ch):
        return N.Cmp(field=ch[0], op="STARTS", value=ch[1])

    def contains_cmp(self, ch):
        return N.Cmp(field=ch[0], op="CONTAINS", value=ch[1])

    def and_expr(self, ch):
        left, right = ch[0], ch[1]
        # flatten nested AND
        subs = (left.subs if isinstance(left, N.And) else [left])
        subs = subs + [right]
        return N.And(subs=subs)

    def or_expr(self, ch):
        left, right = ch[0], ch[1]
        subs = (left.subs if isinstance(left, N.Or) else [left])
        subs = subs + [right]
        return N.Or(subs=subs)

    def not_expr(self, ch):
        return N.Not(sub=ch[0])

    def cond_true(self, _):
        return N.TrueCond()

    # ══════════════════════════════════════════════════════════
    # §9  Graph Definition
    # ══════════════════════════════════════════════════════════

    def mode_live(self, _):    return "live"
    def mode_static(self, _):  return "static"
    def g_hetero(self, _):     return ("hetero", True)
    def g_directed(self, _):   return ("directed", True)
    def g_undirected(self, _): return ("directed", False)

    def create_graph(self, ch):
        mode = ch[0]
        name = _tok(ch[1])
        directed, hetero, where = True, True, None
        for item in ch[2:]:
            if isinstance(item, tuple):
                if item[0] == "directed":
                    directed = item[1]
                elif item[0] == "hetero":
                    hetero = True
                elif item[0] == "where":
                    where = item[1]
        return N.CreateGraph(name=name, mode=mode, directed=directed,
                             heterogeneous=hetero, where=where)

    def use_graph(self, ch):      return N.UseGraph(name=_tok(ch[0]))
    def show_graphs(self, _):     return N.ShowGraphs()
    def describe_graph(self, ch): return N.DescribeGraph(name=_tok(ch[0]))
    def drop_graph(self, ch):     return N.DropGraph(name=_tok(ch[0]))

    def properties_clause(self, fields):
        return ("props", list(fields))

    def unwind_clause(self, ch):
        return ("unwind", ch[0], _tok(ch[1]))

    def e_directed(self, _):   return ("edir", True)
    def e_undirected(self, _): return ("edir", False)

    def map_node(self, ch):
        node_type = _tok(ch[0])
        collection = _tok(ch[1])
        key_path = ch[2]
        props, where = [], None
        for item in ch[3:]:
            if isinstance(item, tuple):
                if item[0] == "props":
                    props = item[1]
                elif item[0] == "where":
                    where = item[1]
        return N.MapNode(node_type=node_type, collection=collection,
                         key_path=key_path, properties=props, where=where)

    def map_edge(self, ch):
        edge_type = _tok(ch[0])
        collection = _tok(ch[1])

        unwind_path, unwind_alias = None, None
        directed, props = True, []

        # ترتیب بعد از collection: [unwind?] source_path source_type target_path target_type [dir?] [props?]
        rest = list(ch[2:])
        idx = 0
        if rest and isinstance(rest[0], tuple) and rest[0][0] == "unwind":
            unwind_path, unwind_alias = rest[0][1], rest[0][2]
            idx = 1

        source_path = rest[idx]
        source_type = _tok(rest[idx + 1])
        target_path = rest[idx + 2]
        target_type = _tok(rest[idx + 3])

        for item in rest[idx + 4:]:
            if isinstance(item, tuple):
                if item[0] == "edir":
                    directed = item[1]
                elif item[0] == "props":
                    props = item[1]

        return N.MapEdge(edge_type=edge_type, collection=collection,
                         source_path=source_path, source_node_type=source_type,
                         target_path=target_path, target_node_type=target_type,
                         directed=directed, properties=props,
                         unwind_path=unwind_path, unwind_alias=unwind_alias)

    # ══════════════════════════════════════════════════════════
    # §10  GML
    # ══════════════════════════════════════════════════════════

    def b_verbose(self, _):    return "verbose"
    def b_nodes_only(self, _): return "nodes_only"
    def b_edges_only(self, _): return "edges_only"

    def build_graph(self, ch):
        name = _tok(ch[0])
        option = ch[1] if len(ch) > 1 else None
        return N.BuildGraph(name=name, option=option)

    def render_graph(self, ch):
        return N.RenderGraph(name=_tok(ch[0]))

    def refresh_graph(self, ch):
        name = _tok(ch[0])
        hours = int(_tok(ch[1])) if len(ch) > 1 else None
        return N.RefreshGraph(name=name, every_hours=hours)

    def compact_graph(self, ch):
        return N.CompactGraph(name=_tok(ch[0]))

    def graph_wal_status(self, ch):
        return N.GraphWalStatus(name=_tok(ch[0]))

    def purge_wal(self, ch):
        return N.PurgeWal(name=_tok(ch[0]))

    def replay_wal(self, ch):
        return N.ReplayWal(name=_tok(ch[0]))

    def graph_status(self, ch):
        return N.GraphStatus(name=_tok(ch[0]))

    def graph_stats(self, ch):
        return N.GraphStats(name=_tok(ch[0]))

    def snapshot_graph(self, ch):
        return N.SnapshotGraph(name=_tok(ch[0]), into=_tok(ch[1]))

    # ══════════════════════════════════════════════════════════
    # §11  Traversal
    # ══════════════════════════════════════════════════════════

    def d_out(self, _):  return "out"
    def d_in(self, _):   return "in"
    def d_both(self, _): return "both"

    def edge_sel(self, ch):
        tok = _tok(ch[0])
        return None if tok == "*" else tok

    def traverse_stmt(self, ch):
        node_type = _tok(ch[0])
        node_id = ch[1]
        direction = ch[2]
        edge_type = ch[3]
        depth = int(_tok(ch[4]))
        limit = int(_tok(ch[5])) if len(ch) > 5 else 100
        return N.Traverse(node_type=node_type, node_id=node_id,
                          direction=direction, edge_type=edge_type,
                          depth=depth, limit=limit)

    def get_node(self, ch):
        return N.GetNode(node_type=_tok(ch[0]), node_id=ch[1])

    def edge_exists(self, ch):
        # ch = [src_type, src_id, ARROW_L, edge_type, ARROW_R, dst_type, dst_id]
        vals = [c for c in ch if not (isinstance(c, Token) and c.type in ("ARROW_L", "ARROW_R"))]
        return N.EdgeExists(src_type=_tok(vals[0]), src_id=vals[1],
                            edge_type=_tok(vals[2]),
                            dst_type=_tok(vals[3]), dst_id=vals[4])

    # ══════════════════════════════════════════════════════════
    # §12  Algorithms
    # ══════════════════════════════════════════════════════════

    def with_param(self, ch):
        return (_tok(ch[0]), ch[1])

    def with_clause(self, params):
        return ("with", dict(params))

    def run_lock(self, ch):
        algo = _tok(ch[0])
        graph = _tok(ch[1])
        params, limit = {}, None
        for item in ch[2:]:
            if isinstance(item, tuple) and item[0] == "with":
                params = item[1]
            elif isinstance(item, Token):
                limit = int(_tok(item))
        return N.RunLock(algo=algo, graph=graph, params=params, limit=limit)

    def run_job(self, ch):
        algo = _tok(ch[0])
        graph = _tok(ch[1])
        params, top = {}, None
        for item in ch[2:]:
            if isinstance(item, tuple) and item[0] == "with":
                params = item[1]
            elif isinstance(item, Token):
                top = int(_tok(item))
        return N.RunJob(algo=algo, graph=graph, params=params, returns_top=top)

    def job_status(self, ch):
        return N.JobStatus(job_id=ch[0])

    def job_result(self, ch):
        return N.JobResult(job_id=ch[0])

    def job_cancel(self, ch):
        return N.JobCancel(job_id=ch[0])

    def show_jobs(self, ch):
        graph = _tok(ch[0]) if ch else None
        return N.ShowJobs(graph=graph)

    # ══════════════════════════════════════════════════════════
    # §13  SYS
    # ══════════════════════════════════════════════════════════

    def system_status(self, _): return N.SystemStatus()
    def system_info(self, _):   return N.SystemInfo()


def _num(s: str):
    """رشته عددی → int یا float"""
    if "." in s or "e" in s.lower():
        return float(s)
    return int(s)
