"""
test.py — تست کامل NexoraDB از Python
══════════════════════════════════════════════════════════════

چطور اجرا کنی:
─────────────────────────────────────────────────────────────
مرحله ۱: build کردن nexoradb.so

    cd ~/NexoraDB
    # cmake-build-debug را پاک و از نو بساز:
    rm -rf cmake-build-debug
    mkdir cmake-build-debug && cd cmake-build-debug

    cmake .. \\
        -DNEXORA_BUILD_GRAPH=ON \\
        -DNEXORA_BUILD_PYTHON=ON \\
        -DCMAKE_BUILD_TYPE=Debug

    make nexoradb_py -j$(nproc)
    # یا make -j$(nproc) برای همه چیز

مرحله ۲: پیدا کردن .so

    find . -name "nexoradb*.so"
    # معمولاً: ./bindings/nexoradb.cpython-3xx-linux-gnu.so

مرحله ۳: کپی .so به کنار test.py

    cp ./bindings/nexoradb.cpython-*.so .

مرحله ۴: اجرا

    python3 test.py
══════════════════════════════════════════════════════════════
"""

import sys
import os
import json
import shutil

# ── اطمینان از اینکه nexoradb.so در همین پوشه است ──
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)

try:
    import nexoradb
    print(f"✅ nexoradb imported — version={nexoradb.__version__}  graph={nexoradb.GRAPH_ENABLED}")
except ImportError as e:
    print(f"""
❌ نمی‌توان nexoradb را import کرد: {e}

مطمئن شو که nexoradb.so در همین پوشه است:
    find ~/NexoraDB/cmake-build-debug -name "nexoradb*.so"
    cp <path_to_so> {HERE}/

اگر هنوز build نشده:
    cd ~/NexoraDB/cmake-build-debug
    cmake .. -DNEXORA_BUILD_GRAPH=ON -DNEXORA_BUILD_PYTHON=ON
    make nexoradb_py -j$(nproc)
""")
    sys.exit(1)

# ── پاک‌سازی دیتابیس قبلی ──
DB_PATH   = "/tmp/nexora_py_test"
GRAPH_DIR = "/tmp/nexora_py_graph"
shutil.rmtree(DB_PATH,   ignore_errors=True)
shutil.rmtree(GRAPH_DIR, ignore_errors=True)

def sep(title): print(f"\n══ {title} ══")
def ok(label, r):
    mark = "✅" if r.success else "❌"
    data = r.data[:80] if r.data else ""
    err  = f"  ERR: {r.error_msg}" if r.error_msg else ""
    print(f"  {mark} {label:40s}  {data}{err}")

def bytes_human(value):
    units = ["B", "KB", "MB", "GB", "TB"]
    size = float(value)
    for unit in units:
        if size < 1024 or unit == units[-1]:
            return f"{size:.2f} {unit}"
        size /= 1024


# ══════════════════════════════════════════════════════════════
# §1  DocEngine — Document Store
# ══════════════════════════════════════════════════════════════

sep("1. DocEngine Init")
engine = nexoradb.DocEngine(DB_PATH)
print(f"  is_healthy: {engine.is_healthy()}")

# ── Collections ──────────────────────────────────────────────
sep("2. Collections")

ok("CreateCollection users", engine.create_collection("users"))
ok("CreateCollection posts",   engine.create_collection("posts"))
ok("CreateCollection follows", engine.create_collection("follows"))
ok("CreateCollection dup(fail)",engine.create_collection("users"))  # باید fail بشه

print(f"  CollectionExists(users): {engine.collection_exists('users')}")
print(f"  ListCollections: {engine.list_collections()}")

# ── Internal DB Users ─────────────────────────────────────────
sep("2.1 Internal DB Users (Hidden System Collection)")

root_user = json.dumps({
    "_id": "usr_root",
    "username": "root",
    "email": "admin@example.com",
    "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$root-test-hash",
    "role": "admin",
    "first_name": "Database",
    "last_name": "Administrator",
    "status": "active",
    "created_at": 1782560000000,
    "updated_at": 1782560000000,
    "last_login_at": None,
})
app_user = json.dumps({
    "_id": "usr_app_01",
    "username": "analytics_app",
    "email": None,
    "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$app-test-hash",
    "role": "application",
    "first_name": None,
    "last_name": None,
    "status": "active",
    "created_at": 1782560000000,
    "updated_at": 1782560000000,
    "last_login_at": None,
})
app_user_updated = json.dumps({
    "_id": "usr_app_01",
    "username": "analytics_app",
    "email": None,
    "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$app-test-hash-2",
    "role": "application",
    "first_name": None,
    "last_name": None,
    "status": "disabled",
    "created_at": 1782560000000,
    "updated_at": 1782561000000,
    "last_login_at": None,
})

ok("Create internal root", engine.create_internal_user(root_user))
ok("Create internal app user", engine.create_internal_user(app_user))
ok("Get internal root", engine.get_internal_user("root"))
ok("Update internal app user", engine.update_internal_user("analytics_app", app_user_updated))
ok("Soft-delete internal app user", engine.delete_internal_user("analytics_app"))

deleted_app = engine.get_internal_user("analytics_app")
ok("Get deleted internal app user", deleted_app)
if deleted_app.success:
    print(f"  deleted status: {json.loads(deleted_app.data).get('status')}")

ok("Public create reserved collection (must FAIL)",
   engine.create_collection("__nexora_internal_users"))
ok("Public find reserved collection (must FAIL)",
   engine.find_many("__nexora_internal_users", nexoradb.Condition()))
print(f"  Hidden from ListCollections: {'__nexora_internal_users' not in engine.list_collections()}")

# ── Insert ───────────────────────────────────────────────────
sep("3. InsertOne / InsertMany")

ok("Insert u1", engine.insert_one("users",
                                  '{"_id":"u1","username":"ali","email":"ali@ex.com","age":28}'))
ok("Insert u2", engine.insert_one("users",
                                  '{"_id":"u2","username":"sara","email":"sara@ex.com","age":25}'))
ok("Insert u3", engine.insert_one("users",
                                  '{"_id":"u3","username":"reza","email":"reza@ex.com","age":32}'))

# بدون _id → UUID خودکار
r_auto = engine.insert_one("users",
                           '{"username":"auto_user","email":"auto@ex.com","age":20}')
ok("Insert auto-id", r_auto)
print(f"  auto-generated id: {r_auto.data}")

ok("InsertMany posts", engine.insert_many("posts", [
    '{"_id":"p1","title":"Hello NexoraDB","author_id":"u1","likes":0,"liked_by":["u2","u3"]}',
    '{"_id":"p2","title":"NoSQL in C++",  "author_id":"u1","likes":5}',
    '{"_id":"p3","title":"Bobs post",     "author_id":"u2","likes":2}',
]))

ok("InsertMany follows", engine.insert_many("follows", [
    '{"_id":"f1","from_id":"u1","to_id":"u2","since":1700000000}',
    '{"_id":"f2","from_id":"u1","to_id":"u3","since":1700001000}',
    '{"_id":"f3","from_id":"u2","to_id":"u3","since":1700002000}',
]))

# ── FindById ─────────────────────────────────────────────────
sep("4. FindById")

ok("FindById u1", engine.find_by_id("users","u1"))
ok("FindById notexist", engine.find_by_id("users","u999"))  # fail

# ── FindMany ─────────────────────────────────────────────────
sep("5. FindMany")

# شرط EQ
c_author = nexoradb.Condition.leaf(
    "author_id", nexoradb.Op.EQ, "u1", nexoradb.ValueType.String)
ok("FindMany posts by u1", engine.find_many("posts", c_author))

# شرط GT
c_age = nexoradb.Condition.leaf(
    "age", nexoradb.Op.GTE, "25", nexoradb.ValueType.Int64)
ok("FindMany users age>=25", engine.find_many("users", c_age))

# AND
c_and = nexoradb.Condition.and_([
    nexoradb.Condition.leaf("age", nexoradb.Op.GTE, "25", nexoradb.ValueType.Int64),
    nexoradb.Condition.leaf("age", nexoradb.Op.LTE, "35", nexoradb.ValueType.Int64),
])
ok("FindMany users 25<=age<=35", engine.find_many("users", c_and))

# OR
c_or = nexoradb.Condition.or_([
    nexoradb.Condition.leaf("username", nexoradb.Op.EQ, "ali",  nexoradb.ValueType.String),
    nexoradb.Condition.leaf("username", nexoradb.Op.EQ, "sara", nexoradb.ValueType.String),
])
ok("FindMany ali OR sara", engine.find_many("users", c_or))

# IN
c_in = nexoradb.Condition.in_("username", ["ali","reza"])
ok("FindMany username IN [ali,reza]", engine.find_many("users", c_in))

# limit + skip
ok("FindMany posts limit=2 skip=1", engine.find_many("posts", nexoradb.Condition(), 2, 1))

# ── Count / Exists ────────────────────────────────────────────
sep("6. Count / Exists")

ok("Count all users",  engine.count("users",  nexoradb.Condition()))
ok("Count all posts",  engine.count("posts",  nexoradb.Condition()))
ok("Count posts likes>0", engine.count("posts",
                                       nexoradb.Condition.leaf("likes",nexoradb.Op.GT,"0",nexoradb.ValueType.Int64)))

print(f"  GetCollectionSize(posts): {engine.get_collection_size('posts')}")

ok("Exists ali",   engine.exists("users",
                                 nexoradb.Condition.leaf("username",nexoradb.Op.EQ,"ali",nexoradb.ValueType.String)))
ok("Exists ghost", engine.exists("users",
                                 nexoradb.Condition.leaf("username",nexoradb.Op.EQ,"ghost",nexoradb.ValueType.String)))

# ── UpdateById ────────────────────────────────────────────────
sep("7. UpdateById / UpdateMany")

spec = nexoradb.UpdateSpec()
spec.inc("likes", "10", nexoradb.UpdateValueType.Int64)
spec.set("updated", "true")
ok("UpdateById p1 likes+=10", engine.update_by_id("posts","p1",spec))
ok("FindById p1 after update", engine.find_by_id("posts","p1"))

spec2 = nexoradb.UpdateSpec()
spec2.push("tags","trending")
ok("UpdateById p2 push tag", engine.update_by_id("posts","p2",spec2))

c_u1 = nexoradb.Condition.leaf("author_id",nexoradb.Op.EQ,"u1",nexoradb.ValueType.String)
spec3 = nexoradb.UpdateSpec()
spec3.inc("likes","1",nexoradb.UpdateValueType.Int64)
ok("UpdateMany posts by u1", engine.update_many("posts",c_u1,spec3))

# ── Index & FK ────────────────────────────────────────────────
sep("8. Index & Foreign Key")

idx = nexoradb.IndexDefinition()
idx.index_name = "idx_author"
idx.fields     = ["author_id"]
idx.type       = nexoradb.IndexType.SingleField
ok("CreateIndex idx_author", engine.create_index("posts",idx))

fk = nexoradb.ForeignKeyDefinition()
fk.fk_name        = "fk_post_author"
fk.local_field    = "author_id"
fk.ref_collection = "users"
fk.ref_field      = "_id"
ok("AddForeignKey", engine.add_foreign_key("posts",fk))

ok("Insert invalid FK (must FAIL)",
   engine.insert_one("posts", '{"_id":"p_bad","title":"bad","author_id":"u99"}'))
ok("Insert valid FK (must OK)",
   engine.insert_one("posts", '{"_id":"p5","title":"FK ok","author_id":"u1","likes":0}'))

# ── Delete ────────────────────────────────────────────────────
sep("9. Delete")

ok("DeleteById p3", engine.delete_by_id("posts","p3"))
ok("FindById p3 (must fail)", engine.find_by_id("posts","p3"))

c_zero = nexoradb.Condition.leaf("likes",nexoradb.Op.EQ,"0",nexoradb.ValueType.Int64)
ok("DeleteMany likes=0", engine.delete_many("posts",c_zero))
ok("Count posts after delete", engine.count("posts",nexoradb.Condition()))

# ── Transaction ───────────────────────────────────────────────
sep("10. Transaction")

ok("CreateCollection comments", engine.create_collection("comments"))

tx = engine.begin_transaction()
if tx and tx.is_valid():
    # InsertOneTx — داخل transaction (نه insert_one معمولی)
    engine.insert_one_tx(tx, "comments",
                         '{"post_id":"p1","text":"tx test","user_id":"u2"}')
    commit = engine.commit_transaction(tx)
    ok("Tx commit", commit)
    ok("Count comments after Tx (expect 1)", engine.count("comments",nexoradb.Condition()))
else:
    print("  ❌ begin_transaction failed")

# Rollback — insert داخل tx باید برگردد
tx2 = engine.begin_transaction()
if tx2 and tx2.is_valid():
    engine.insert_one_tx(tx2, "comments",
                         '{"post_id":"p1","text":"rollback test","user_id":"u1"}')
    engine.rollback_transaction(tx2)
    print("  ✅ Rollback")
    ok("Count after rollback (still 1)", engine.count("comments",nexoradb.Condition()))

# ── LookupJoin ────────────────────────────────────────────────
sep("11. LookupJoin")

join = engine.lookup_join("posts","author_id","users","_id",
                          nexoradb.Condition(), 5)
print(f"  LookupJoin: success={join['success']} records={len(join['records'])}")
for rec in join['records'][:2]:
    d = json.loads(rec)
    print(f"    post={d.get('title','')} author_joined={bool('__joined__' in d)}")

# ── IterateCollection ─────────────────────────────────────────
sep("12. IterateCollection (Graph API)")

print("  Users:")
def show_user(doc_id, bson):
    print(f"    id={doc_id}  doc={bson[:60]}")
    return True
engine.iterate_collection("users", show_user)

print("  Follows:")
engine.iterate_collection("follows",
                          lambda id, bson: print(f"    {id}: {bson}") or True)


# ══════════════════════════════════════════════════════════════
# §2  GraphEngine (فقط اگر GRAPH_ENABLED)
# ══════════════════════════════════════════════════════════════

if not nexoradb.GRAPH_ENABLED:
    print("\n[GraphEngine] Build with -DNEXORA_BUILD_GRAPH=ON to enable")
    print("\n✅ DocEngine tests completed!")
    sys.exit(0)

sep("13. GraphManager — Define & Build")

gm = nexoradb.GraphManager(engine, GRAPH_DIR)
gm.startup()

# ── تعریف گراف ──
gdef = nexoradb.GraphDefinition()
gdef.name                  = "social"
gdef.mode                  = nexoradb.GraphMode.Live
gdef.directed              = True
gdef.heterogeneous         = True
gdef.auto_build_on_startup = False

# Node: User از users
nm_user = nexoradb.NodeMappingDef()
nm_user.node_type  = "User"
nm_user.collection = "users"
nm_user.key_path   = "_id"
nm_user.properties = ["username","age"]

# Node: Post از posts
nm_post = nexoradb.NodeMappingDef()
nm_post.node_type  = "Post"
nm_post.collection = "posts"
nm_post.key_path   = "_id"
nm_post.properties = ["title","likes"]

# Edge: FOLLOWS — collection جداگانه
em_follows = nexoradb.EdgeMappingDef()
em_follows.edge_type        = "FOLLOWS"
em_follows.collection       = "follows"
em_follows.source_path      = "from_id"
em_follows.source_node_type = "User"
em_follows.target_path      = "to_id"
em_follows.target_node_type = "User"
em_follows.directed         = True

# Edge: AUTHORED — FK در سند
em_authored = nexoradb.EdgeMappingDef()
em_authored.edge_type        = "AUTHORED"
em_authored.collection       = "posts"
em_authored.source_path      = "author_id"
em_authored.source_node_type = "User"
em_authored.target_path      = "_id"
em_authored.target_node_type = "Post"
em_authored.directed         = True

# Edge: LIKES — UNWIND array
em_likes = nexoradb.EdgeMappingDef()
em_likes.edge_type        = "LIKES"
em_likes.collection       = "posts"
em_likes.source_path      = "liker_id"
em_likes.source_node_type = "User"
em_likes.target_path      = "_id"
em_likes.target_node_type = "Post"
em_likes.directed         = True
uw = nexoradb.UnwindConfig()
uw.array_path = "liked_by"
uw.alias      = "liker_id"
em_likes.unwind = uw

# ── راه‌حل صحیح: list را یک‌جا assign کن ──
# append() در pybind11 روی کپی کار می‌کند نه object اصلی
# باید list کامل را یک‌جا set کرد
gdef.node_mappings = nexoradb.NodeMappingList([nm_user, nm_post])
gdef.edge_mappings = nexoradb.EdgeMappingList([em_follows, em_authored, em_likes])

created = gm.create_graph(gdef)
print(f"  createGraph(social): {'✅' if created else '❌'}")

# ── Build گراف ──
br = gm.build_graph("social")
print(f"  buildGraph: success={br.success} nodes={br.nodes_built} "
      f"edges={br.edges_built} time={br.elapsed_ms:.1f}ms")
if not br.success:
    print(f"  error: {br.error_msg}")

# ── آمار ──
sep("14. Graph Stats")
stats = gm.get_stats("social")
print(f"  active_nodes={stats.active_nodes}  active_edges={stats.active_edges}  "
      f"version={stats.version}")

# ── Traversal ─────────────────────────────────────────────────
sep("15. Traversal — LiveGraph")

def show_nbrs(label, ids):
    print(f"  {label}: {ids}")

show_nbrs("u1 FOLLOWS (out)",     gm.neighbors("social","u1","out","FOLLOWS",50))
show_nbrs("u1 FOLLOWED BY (in)",  gm.neighbors("social","u1","in", "FOLLOWS",50))
show_nbrs("u1 AUTHORED (out)",    gm.neighbors("social","u1","out","AUTHORED",50))
show_nbrs("u1 LIKES posts (out)", gm.neighbors("social","u1","out","LIKES",50))
show_nbrs("ALL out from u1",      gm.neighbors("social","u1","out","",100))

print(f"  hasEdge u1→u2 FOLLOWS: {gm.has_edge('social','u1','u2','FOLLOWS')}")
print(f"  hasEdge u2→u1 FOLLOWS: {gm.has_edge('social','u2','u1','FOLLOWS')}")

# ── Built-in Algorithms ───────────────────────────────────────
sep("15.1 Built-in Graph Algorithms")

mf = gm.run_mutual_friends("social", ["u1", "u2", "FOLLOWS"])
print(f"  MutualFriends: success={mf.success} json={mf.result_json} "
      f"time={mf.elapsed_ms:.3f}ms")
assert mf.success, mf.error_msg
mf_data = json.loads(mf.result_json)
assert mf_data["count"] == 1, mf_data
assert mf_data["mutual_friends"] == ["u3"], mf_data

cc = gm.run_connected_components("social", [])
print(f"  ConnectedComponents: success={cc.success} json={cc.result_json} "
      f"time={cc.elapsed_ms:.3f}ms")
assert cc.success, cc.error_msg
cc_data = json.loads(cc.result_json)
assert cc_data["total_components"] >= 1, cc_data
assert cc_data["total_nodes"] >= 3, cc_data
assert cc_data["largest_component_size"] >= 3, cc_data

mc = gm.run_most_connected("social", ["2", "out", "User"])
print(f"  MostConnected: success={mc.success} json={mc.result_json} "
      f"time={mc.elapsed_ms:.3f}ms")
assert mc.success, mc.error_msg
mc_data = json.loads(mc.result_json)
assert mc_data["metric"] == "out", mc_data
assert mc_data["limit"] == 2, mc_data
assert len(mc_data["results"]) == 2, mc_data
assert mc_data["results"][0]["id"] == "u1", mc_data
assert mc_data["results"][0]["out"] >= mc_data["results"][1]["out"], mc_data

ns = gm.run_network_stats("social", ["full"])
print(f"  NetworkStats: success={ns.success} json={ns.result_json} "
      f"time={ns.elapsed_ms:.3f}ms")
assert ns.success, ns.error_msg
ns_data = json.loads(ns.result_json)
assert ns_data["mode"] == "full", ns_data
assert ns_data["basic"]["active_nodes"] == stats.active_nodes, ns_data
assert ns_data["basic"]["active_edges"] == stats.active_edges, ns_data
assert ns_data["node_types"]["User"] >= 3, ns_data
assert ns_data["node_types"]["Post"] == 2, ns_data
assert ns_data["edge_types"]["FOLLOWS"] == 3, ns_data
assert "degree" in ns_data, ns_data

cd = gm.run_community_detection("social", ["10", "2", "members", "User"])
print(f"  CommunityDetection: success={cd.success} json={cd.result_json} "
      f"time={cd.elapsed_ms:.3f}ms")
assert cd.success, cd.error_msg
cd_data = json.loads(cd.result_json)
assert cd_data["algorithm"] == "label_propagation", cd_data
assert cd_data["total_communities"] >= 1, cd_data
assert cd_data["total_nodes_assigned"] >= 3, cd_data
assert cd_data["summary"]["largest_community_size"] >= 3, cd_data
assert "members" in cd_data["communities"][0], cd_data
assert set(cd_data["communities"][0]["members"]) >= {"u1", "u2", "u3"}, cd_data

ad = gm.run_all_distances("social", ["u1", "", "2", "User"])
print(f"  AllDistances: success={ad.success} json={ad.result_json} "
      f"time={ad.elapsed_ms:.3f}ms")
assert ad.success, ad.error_msg
ad_data = json.loads(ad.result_json)
assert ad_data["mode"] == "sssp", ad_data
assert ad_data["source"] == "u1", ad_data
assert ad_data["max_hops"] == 2, ad_data
dist_by_id = {item["id"]: item["distance"] for item in ad_data["distances"]}
assert dist_by_id["u2"] == 1, ad_data
assert dist_by_id["u3"] == 1, ad_data

gf = gm.run_get_friends("social", ["u1", "10", "FOLLOWS"])
print(f"  GetFriends: success={gf.success} json={gf.result_json} "
      f"time={gf.elapsed_ms:.3f}ms")
assert gf.success, gf.error_msg
gf_data = json.loads(gf.result_json)
assert gf_data["user_id"] == "u1", gf_data
assert gf_data["friend_count"] == 2, gf_data
assert set(gf_data["friends"]) == {"u2", "u3"}, gf_data

ac = gm.run_are_connected("social", ["u1", "u3", "FOLLOWS"])
print(f"  AreConnected: success={ac.success} json={ac.result_json} "
      f"time={ac.elapsed_ms:.3f}ms")
assert ac.success, ac.error_msg
ac_data = json.loads(ac.result_json)
assert ac_data["connected"] is True, ac_data
assert ac_data["hops"] == 1, ac_data

sp = gm.run_shortest_path("social", ["u3", "u1", "FOLLOWS"])
print(f"  ShortestPath: success={sp.success} json={sp.result_json} "
      f"time={sp.elapsed_ms:.3f}ms")
assert sp.success, sp.error_msg
sp_data = json.loads(sp.result_json)
assert sp_data["found"] is True, sp_data
assert sp_data["hops"] == 1, sp_data
assert sp_data["path"] == ["u3", "u1"], sp_data

fs = gm.run_friend_suggestion("social", ["u1", "10", "FOLLOWS"])
print(f"  FriendSuggestion: success={fs.success} json={fs.result_json} "
      f"time={fs.elapsed_ms:.3f}ms")
assert fs.success, fs.error_msg
fs_data = json.loads(fs.result_json)
assert fs_data["user_id"] == "u1", fs_data
assert fs_data["suggestion_count"] == 0, fs_data
assert fs_data["suggestions"] == [], fs_data

bc = gm.run_betweenness_centrality("social", ["3"])
print(f"  BetweennessCentrality: success={bc.success} json={bc.result_json} "
      f"time={bc.elapsed_ms:.3f}ms")
assert bc.success, bc.error_msg
bc_data = json.loads(bc.result_json)
assert bc_data["total_nodes"] == stats.active_nodes, bc_data
assert bc_data["showing"] == 3, bc_data
assert len(bc_data["nodes"]) == 3, bc_data
assert [item["rank"] for item in bc_data["nodes"]] == [1, 2, 3], bc_data

im = gm.run_influence_maximization("social", ["2", "5", "0"])
print(f"  InfluenceMaximization: success={im.success} json={im.result_json} "
      f"time={im.elapsed_ms:.3f}ms")
assert im.success, im.error_msg
im_data = json.loads(im.result_json)
assert im_data["k_seeds"] == 2, im_data
assert im_data["simulations"] == 5, im_data
assert im_data["propagation_prob"] == 0, im_data
assert im_data["estimated_reach"] == 2, im_data
assert len(im_data["seeds"]) == 2, im_data

# ── NexoraQL + Admin backend → parser → pybind → C++ ─────────
ADMIN_SRC = os.path.join(ROOT, "NexoraDB", "src")
if ADMIN_SRC not in sys.path:
    sys.path.insert(0, ADMIN_SRC)

from nexoradb_admin.query_runner import QueryExecuteRequest, execute_query

admin_lock = execute_query(
    engine=engine,
    graph_manager=gm,
    payload=QueryExecuteRequest(
        query=(
            "RUN LOCK GetFriends ON social WITH user='u1', "
            "edge_type='FOLLOWS' LIMIT 10;"
        )
    ),
)
admin_lock_stmt = admin_lock.raw["statements"][0]
assert admin_lock_stmt["success"], admin_lock_stmt
assert set(admin_lock_stmt["result"]["friends"]) == {"u2", "u3"}, admin_lock_stmt

admin_job = execute_query(
    engine=engine,
    graph_manager=gm,
    payload=QueryExecuteRequest(
        query=(
            "RUN JOB InfluenceMaximization ON social "
            "WITH k=2, simulations=5, probability=0;"
        )
    ),
)
admin_job_stmt = admin_job.raw["statements"][0]
assert admin_job_stmt["success"], admin_job_stmt
assert admin_job_stmt["status"] == "done", admin_job_stmt
assert admin_job_stmt["result"]["k_seeds"] == 2, admin_job_stmt
assert admin_job_stmt["result"]["estimated_reach"] == 2, admin_job_stmt
print("  Admin query backend: new LOCK/JOB algorithms passed")

# ── GraphMode.Static باید همان built-in algorithms را پشتیبانی کند ───────
sep("15.2 Built-in Algorithms — GraphMode.Static")

static_def = nexoradb.GraphDefinition()
static_def.name                  = "social_static"
static_def.mode                  = nexoradb.GraphMode.Static
static_def.directed              = True
static_def.heterogeneous         = True
static_def.auto_build_on_startup = False
static_def.node_mappings = nexoradb.NodeMappingList([nm_user, nm_post])
static_def.edge_mappings = nexoradb.EdgeMappingList([em_follows, em_authored, em_likes])

assert gm.create_graph(static_def), "create static graph failed"
static_br = gm.build_graph("social_static")
print(f"  buildGraph(static): success={static_br.success} "
      f"nodes={static_br.nodes_built} edges={static_br.edges_built}")
assert static_br.success, static_br.error_msg

static_mf = gm.run_mutual_friends("social_static", ["u1", "u2", "FOLLOWS"])
assert static_mf.success, static_mf.error_msg
assert json.loads(static_mf.result_json)["mutual_friends"] == ["u3"]

static_mc = gm.run_most_connected("social_static", ["2", "out", "User"])
assert static_mc.success, static_mc.error_msg
assert json.loads(static_mc.result_json)["results"][0]["id"] == "u1"

static_ns = gm.run_network_stats("social_static", ["full"])
assert static_ns.success, static_ns.error_msg
static_ns_data = json.loads(static_ns.result_json)
assert static_ns_data["basic"]["active_nodes"] == stats.active_nodes, static_ns_data
assert static_ns_data["edge_types"]["FOLLOWS"] == 3, static_ns_data

static_cc = gm.run_connected_components("social_static", [])
assert static_cc.success, static_cc.error_msg
assert json.loads(static_cc.result_json)["largest_component_size"] >= 3

static_cd = gm.run_community_detection("social_static", ["10", "2", "members", "User"])
assert static_cd.success, static_cd.error_msg
assert json.loads(static_cd.result_json)["total_nodes_assigned"] >= 3

static_ad = gm.run_all_distances("social_static", ["u1", "", "2", "User"])
assert static_ad.success, static_ad.error_msg
static_dist_by_id = {
    item["id"]: item["distance"]
    for item in json.loads(static_ad.result_json)["distances"]
}
assert static_dist_by_id["u2"] == 1
assert static_dist_by_id["u3"] == 1
print("  Static graph algorithms: all 6 passed")

# ── Live Update ──────────────────────────────────────────────
sep("16. Live Update")

# درج follow جدید → گراف آپدیت می‌شود بدون rebuild
engine.insert_one("follows",
                  '{"_id":"f_live","from_id":"u3","to_id":"u1","since":1700009000}')
gm.on_document_inserted("follows",
                        '{"_id":"f_live","from_id":"u3","to_id":"u1","since":1700009000}')

show_nbrs("u1 FOLLOWED BY after live update",
          gm.neighbors("social","u1","in","FOLLOWS",50))

# ── StaticGraph Snapshot ──────────────────────────────────────
sep("17. StaticGraph Snapshot (JobAlgorithm)")

snap = gm.create_snapshot("social")
if snap:
    s = snap.stats()
    print(f"  Snapshot: nodes={s.node_count}  edges={s.edge_count}  "
          f"version={s.version}")

    # iterate nodes
    print("  Nodes:")
    def show_node(dense_id, type_id):
        print(f"    DenseId={dense_id}  type={snap.node_type_name(type_id)}"
              f"  ext={snap.ext_id(dense_id)}")
        return True
    snap.for_each_node(show_node)

    # neighbors در snapshot
    u1_dense = snap.dense_id("u1")
    INVALID = (1 << 64) - 1  # kInvalidDenseId = UINT64_MAX
    if u1_dense != INVALID:
        nbrs = snap.neighbors(u1_dense, nexoradb.Direction.Out)
        print(f"  u1 OUT neighbors (snapshot): "
              f"{[snap.ext_id(n) for n in nbrs]}")

    # COO export
    opts = nexoradb.GraphExportOptions()
    opts.remap_contiguous = True
    coo = snap.export_coo(opts)
    print(f"  COO export: {len(coo.src)} edges")
    for i in range(len(coo.src)):
        s_ext = snap.ext_id(coo.original_node_ids[coo.src[i]])
        d_ext = snap.ext_id(coo.original_node_ids[coo.dst[i]])
        print(f"    {s_ext} → {d_ext}")

    # CSR export
    csr = snap.export_csr(opts)
    print(f"  CSR rowPtr size: {len(csr.row_ptr)}")

    # node/edge count by type
    print(f"  nodes by type: {snap.node_count_by_type()}")
    print(f"  edges by type: {snap.edge_count_by_type()}")

# ── WAL ───────────────────────────────────────────────────────
sep("18. WAL Status")
wal = gm.get_wal_status("social")
print(f"  total={wal['total_entries']}  pending={wal['pending_entries']}  "
      f"has_pending={wal['has_pending']}")

sep("19. Admin Dashboard Methods")
ram_bytes = engine.get_ram_usage_bytes()
disk_bytes = engine.get_disk_usage_bytes()
recent_wal = gm.get_recent_wal_entries("social")

print(f"  DB RAM usage:  {ram_bytes} bytes ({bytes_human(ram_bytes)})")
print(f"  DB disk usage: {disk_bytes} bytes ({bytes_human(disk_bytes)})")
print(f"  Recent WAL entries ({len(recent_wal)} of max 20):")
for entry in recent_wal:
    print(f"    seq={entry['seq']} op={entry['op']} "
          f"node_or_edge_id={entry['node_or_edge_id']} "
          f"src={entry['src_id']} dst={entry['dst_id']} "
          f"type={entry['type_id']} applied={entry['applied']} "
          f"ts_ms={entry['timestamp_ms']}")

sep("20. listGraphs")
print(f"  graphs: {gm.list_graphs()}")

# ──────────────────────────────────────────────────────────────
print("\n══════════════════════════════════════════════")
print("  ✅ All tests completed successfully!")
print("══════════════════════════════════════════════")
