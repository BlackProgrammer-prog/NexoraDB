<div align="center">

# ⚡ NexoraDB

### A High-Performance Hybrid Document + Graph Database Engine

**C++20 core · RocksDB storage · Live graph projections · Built-in graph algorithms · SQL-like query language**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Python](https://img.shields.io/badge/Python-3.10%2B-3776AB.svg?style=flat&logo=python&logoColor=white)](https://www.python.org/)
[![RocksDB](https://img.shields.io/badge/Storage-RocksDB-orange.svg?style=flat)](https://rocksdb.org/)
[![pybind11](https://img.shields.io/badge/Bindings-pybind11-green.svg?style=flat)](https://github.com/pybind/pybind11)
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg?style=flat)](#-license)
[![FastAPI](https://img.shields.io/badge/FastAPI-0.110.0-009688.svg?style=flat&logo=fastapi)](https://fastapi.tiangolo.com/)

[Features](#-features) •
[Architecture](#-architecture) •
[Installation](#-installation) •
[Quick Start](#-quick-start) •
[NexoraQL](#-nexoraql--the-query-language) •
[Algorithms](#-graph-algorithms) •
[CLI](#-cli--terminal-commands)

</div>

---

## 🎯 What is NexoraDB?

**NexoraDB** is an embedded database engine that unifies two worlds in a single system:

1. **A Document Store** (MongoDB-style) — schemaless JSON documents, secondary indexes, foreign keys, and ACID transactions, all persisted in **RocksDB**.
2. **A Graph Engine** — your documents are *projected* into an in-memory graph in real time. Insert a document, and the graph updates instantly. No ETL, no sync jobs, no second database.

On top of both sits **NexoraQL** — an SQL-like query language that lets you create collections, insert documents, traverse relationships, and run graph algorithms in one unified syntax.

```sql
-- One language. Documents AND graphs.
INSERT INTO users VALUES ('{"_id":"u1","username":"alice","age":28}');

CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
MAP NODE User FROM users KEY _id PROPERTIES username, age;
MAP EDGE FOLLOWS FROM follows SOURCE from_id AS User TARGET to_id AS User;
BUILD GRAPH social;

TRAVERSE User('u1') OUT FOLLOWS DEPTH 2 LIMIT 50;
RUN LOCK MutualFriends ON social WITH user1='u1', user2='u2';
```

### Why NexoraDB?

| Problem | Traditional Approach | NexoraDB |
|---|---|---|
| Documents + relationships | MongoDB **+** Neo4j **+** sync pipeline | One engine, one source of truth |
| Graph freshness | Batch ETL (minutes/hours stale) | **Live** — updates on every insert |
| Analytics blocking OLTP | Read replicas, complex setups | Lock-free **snapshots** for heavy jobs |
| ML/GNN export | Custom exporters | Native **COO / CSR** export for PyTorch Geometric |

---

## ✨ Features

### 📄 Document Engine (`DocEngine`)
- **Full CRUD** — `InsertOne/Many`, `FindById` (O(1)), `FindMany` with rich conditions, `UpdateById/Many`, `DeleteById/Many`
- **Query conditions** — `EQ, NEQ, GT, GTE, LT, LTE, IN, NIN, EXISTS, REGEX, STARTS, CONTAINS` with `AND / OR / NOR / NOT` composition
- **Update operators** — `$set, $inc, $unset, $push, $pull, $addToSet, $currentDate` and more
- **Schema validation** (optional, per collection) with `REQUIRED` / `UNIQUE` field flags
- **Secondary indexes** — single-field, compound, unique
- **Foreign keys** — referential integrity enforced on insert
- **ACID transactions** — via RocksDB `TransactionDB` (begin / commit / rollback)
- **Lookup joins** — MongoDB-style `$lookup` left joins
- **Auto UUID v4** document IDs when `_id` is omitted

### 🕸️ Graph Engine (`GraphManager`)
- **Declarative mapping** — map collections to node types and edges (three styles: edge collections, embedded foreign keys, `UNWIND` over arrays)
- **LiveGraph** — RAM adjacency structure with **sorted chunked adjacency lists**, updated incrementally on every document insert/update/delete
- **StaticGraph snapshots** — immutable, lock-free copies for long-running analytics; OLTP traffic never blocks
- **Write-Ahead Log (WAL)** — crash-safe graph state with replay on startup
- **Heterogeneous graphs** — multiple node types and edge types in one graph
- **ML export** — `exportCOO()` / `exportCSR()` for PyTorch Geometric, DGL, and GPU pipelines
- **Cached degrees** — `out_degree` / `in_degree` are O(1) reads, never counted on the fly

### 🧮 Two-Tier Algorithm Framework
- **`LockAlgorithm`** — fast (<200 ms), node-local queries running on the LiveGraph under a shared read lock
- **`JobAlgorithm`** — heavy, whole-graph analytics running **asynchronously** on a snapshot with a `JobHandle` (poll → result)

### 🔤 NexoraQL
- SQL-like, case-insensitive language built with [**Lark**](https://github.com/lark-parser/lark)
- Covers DDL, DML, transactions, graph definition, traversal, and algorithm execution
- Clean pipeline: **grammar → AST → semantic validation → execution** against the C++ core
- Friendly errors with line/column context

### 🐍 First-Class Python
- Complete **pybind11** bindings (`nexoradb.so`) — GIL released during heavy C++ work
- Pythonic wrapper (`NexoraDB` / `GraphDB`) with MongoDB-style filter dicts
- One-line session: `NexoraQLSession(db_path, graph_dir).execute(sql)`

---

### 🌐 REST API (FastAPI)
- **FastAPI REST API** — full HTTP interface for external applications
- **App token authentication** — HMAC-SHA256 signed tokens with 9 fine-grained scopes
- **Admin dashboard** — React-based management UI (served via `nexoradb dashboard`)
- **Scope-based authorization** — granular access control per endpoint
- **Production-ready** — environment-aware settings with docs disabled in production

## 🌐 REST API (FastAPI)

NexoraDB includes a complete **REST API layer** built with FastAPI, enabling external applications to interact with the database over HTTP.

### Quick API Start

```bash
# Start the API server
nexoradb server

# Or with auto-reload for development
uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
Once running, explore the interactive API documentation at `http://localhost:8000/docs`.

---

### Authentication

All API endpoints require a bearer token. Tokens are created via the admin API:

```bash
# Create an app token (requires admin:apps scope)
curl -X POST http://localhost:8000/api/v1/apps/tokens \
  -H "Authorization: Bearer <admin-token>" \
  -H "Content-Type: application/json" \
  -d '{"appId":"billing-service","scopes":["query:execute"]}'


The response includes a token in the format nxapp_{header}.{payload}.{signature}. Use it in all subsequent requests:

```bash
curl -X POST http://localhost:8000/api/v1/query \
  -H "Authorization: Bearer nxapp_..." \
  -H "Content-Type: application/json" \
  -d '{"query":"SELECT * FROM users LIMIT 10;"}'

---

### Available Scopes

| Scope | Description |
|-------|-------------|
| `query:execute` | Execute NexoraQL queries |
| `documents:read` | Read documents from collections |
| `documents:write` | Insert, update, and delete documents |
| `collections:read` | List collections and check existence |
| `collections:write` | Create, drop, and modify collections |
| `graphs:read` | Read graph data and statistics |
| `graphs:write` | Modify graph structure (nodes, edges) |
| `monitoring:read` | Read system metrics and health status |
| `admin:apps` | Create and manage application tokens |

### API Endpoints

| Method | Endpoint | Description | Required Scope |
|--------|----------|-------------|----------------|
| POST | `/api/v1/query` | Execute NexoraQL query | `query:execute` |
| POST | `/api/v1/documents/{collection}` | Insert one document | `documents:write` |
| GET | `/api/v1/documents/{collection}/{id}` | Find document by ID | `documents:read` |
| PATCH | `/api/v1/documents/{collection}/{id}` | Update document by ID | `documents:write` |
| DELETE | `/api/v1/documents/{collection}/{id}` | Delete document by ID | `documents:write` |
| POST | `/api/v1/collections` | Create a collection | `collections:write` |
| GET | `/api/v1/collections` | List all collections | `collections:read` |
| DELETE | `/api/v1/collections/{collection}` | Drop a collection | `collections:write` |
| POST | `/api/v1/graph/{name}/node` | Add a node to a graph | `graphs:write` |
| GET | `/api/v1/graph/{name}/node/{id}/neighbors` | Get node neighbors | `graphs:read` |
| GET | `/api/v1/graph/{name}/stats` | Get graph statistics | `graphs:read` |
| POST | `/api/v1/algorithms/lock` | Run lightweight LockAlgorithm | `graphs:read` |
| POST | `/api/v1/algorithms/job` | Run heavy JobAlgorithm (async) | `graphs:read` |
| GET | `/api/v1/algorithms/job/{id}/status` | Check job status | `graphs:read` |
| POST | `/api/v1/apps/tokens` | Create an app token | `admin:apps` |
| GET | `/api/v1/system/health` | Health check | *none (public)* |
| GET | `/api/v1/system/metrics` | System metrics | `monitoring:read` |

For complete API reference, see [`docs/fastapi/api_reference.md`](docs/fastapi/api_reference.md).

### Production Security

| Setting | Requirement |
|---------|-------------|
| `API_TOKEN_SECRET` | Must be at least 32 characters |
| `DEBUG` | Must be `False` |
| `ENVIRONMENT` | Must be `production` |
| OpenAPI docs | Disabled (`/docs` returns 404) |
| CORS origins | Must be explicitly restricted |

---

## 🏗 Architecture

```
┌───────────────────────────────────────────────────────────────┐
│                     Applications / Dashboard                  │
│                  (FastAPI · React · CLI · Notebooks)          │
├───────────────────────────────────────────────────────────────┤
│  NexoraQL  (Python · Lark)                                    │
│  grammar → parser → AST → semantic validator → executor       │
├───────────────────────────────────────────────────────────────┤
│  nexoradb.so  (pybind11 bindings)                             │
├───────────────────────────────┬───────────────────────────────┤
│         GraphManager          │           DocEngine           │
│  ┌─────────────────────────┐  │   CRUD · Index · FK · Tx      │
│  │ LiveGraph (RAM)         │  │   Schema · LookupJoin         │
│  │  sorted adjacency       │◄─┤   IterateCollection           │
│  ├─────────────────────────┤  ├───────────────────────────────┤
│  │ StaticGraph (snapshots) │  │          QueryLayer           │
│  ├─────────────────────────┤  │  Condition · UpdateSpec       │
│  │ GraphWAL (.nexl)        │  │  Evaluator (Match / Apply)    │
│  │ GraphStorage (.nex)     │  │                               │
│  └─────────────────────────┘  │                               │
├───────────────────────────────┴───────────────────────────────┤
│                          RocksDB                              │
│              ★ the single source of truth ★                   │
└───────────────────────────────────────────────────────────────┘
```

**Core design rule:** RocksDB is the *only* source of truth. The LiveGraph is a disposable in-memory projection — it can always be rebuilt from documents with `BUILD GRAPH`.

**Dependency chain:** `nexora_query → nexora_core → nexora_graph → nexoradb.so / NexoraDB (test exe)`

---

## 📦 Installation

### Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| C++ compiler | GCC 11+ / Clang 14+ / MSVC 2022 | C++20 required |
| CMake | ≥ 3.22 | |
| [vcpkg](https://github.com/microsoft/vcpkg) | latest | dependency manager |
| Python | ≥ 3.10 | for bindings & NexoraQL |

### 1. Clone & install C++ dependencies

```bash
git clone https://github.com/your-org/nexoradb.git
cd nexoradb

# vcpkg resolves rocksdb, fmt, and pybind11 from vcpkg.json
export VCPKG_ROOT=/path/to/vcpkg
```

### 2. Build

```bash
mkdir build && cd build

cmake .. \
    -DNEXORA_BUILD_GRAPH=ON \
    -DNEXORA_BUILD_PYTHON=ON \
    -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
```

This produces:

| Artifact | Description |
|---|---|
| `NexoraDB` | C++ test executable (full engine self-test) |
| `nexoradb.cpython-3xx.so` | Python module |

### 3. Install the Python side

```bash
# The ONLY Python dependency for NexoraQL:
pip install lark

# Make the compiled module importable (copy next to your scripts, or add to PYTHONPATH)
cp build/nexoradb.cpython-*.so .
```

### Build options

| Flag | Default | Effect |
|---|---|---|
| `NEXORA_BUILD_GRAPH` | `ON` | Build the graph engine |
| `NEXORA_BUILD_PYTHON` | `ON` | Build pybind11 bindings |
| `NEXORA_DEBUG_LOG` | `OFF` | Verbose engine logging |
| `NEXORA_ENABLE_ASAN` | `OFF` | AddressSanitizer builds |

---

## 🚀 Quick Start

### Option A — NexoraQL (recommended)

```python
from nexoraql import NexoraQLSession

session = NexoraQLSession("/var/data/mydb", "./graph_data")

results = session.execute('''
    -- Documents
    CREATE COLLECTION users;
    CREATE COLLECTION follows;

    INSERT INTO users VALUES ('{"_id":"u1","username":"alice","age":28}');
    INSERT INTO users VALUES ('{"_id":"u2","username":"bob","age":31}');
    INSERT INTO follows VALUES ('{"_id":"f1","from_id":"u1","to_id":"u2"}');

    SELECT username, age FROM users WHERE age > 25 LIMIT 10;

    -- Graph
    CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
    USE GRAPH social;
    MAP NODE User FROM users KEY _id PROPERTIES username, age;
    MAP EDGE FOLLOWS FROM follows SOURCE from_id AS User TARGET to_id AS User;
    BUILD GRAPH social;

    TRAVERSE User('u1') OUT FOLLOWS DEPTH 2 LIMIT 50;
    EDGE EXISTS User('u1') -[FOLLOWS]-> User('u2');
    GRAPH STATS social;
''')

for r in results:
    print(r)

session.close()
```

### Option B — Direct Python API

```python
import nexoradb

# ── Document store ──
db = nexoradb.DocEngine("/var/data/mydb")

db.create_collection("users")
r = db.insert_one("users", '{"_id":"u1","username":"alice","age":28}')

cond = nexoradb.Condition.leaf("age", nexoradb.Op.GT, "18",
                               nexoradb.ValueType.Int64)
result = db.find_many("users", cond, limit=10)
print(result.data)   # '[{"_id":"u1",...}]'

# ── Graph ──
gm = nexoradb.GraphManager(db, "./graph_data")
gm.startup()

friends = gm.neighbors("social", "u1", "out", "FOLLOWS", limit=50)
snap = gm.create_snapshot("social")          # lock-free analytics copy
coo  = snap.export_coo()                     # → PyTorch Geometric edge_index
```

### Option C — C++

```cpp
#include "core/DocEngine.h"
#include "graph/GraphManager.h"

nexora::core::DocEngine engine("/var/data/mydb");
nexora::graph::GraphManager gm(&engine, "./graph_data");
gm.startup();

auto build = gm.buildGraph("social");
// build.nodes_built, build.edges_built, build.elapsed_ms
```

---

## 🔤 NexoraQL — The Query Language

NexoraQL is case-insensitive, statements end with `;`, comments use `--` or `/* */`.

### Collections (DDL)

```sql
CREATE COLLECTION users (
    username STRING REQUIRED UNIQUE,
    email    STRING REQUIRED,
    age      INT32
) STRICT;

CREATE COLLECTION posts;                 -- schemaless
DROP COLLECTION old_data;
SHOW COLLECTIONS;
DESCRIBE COLLECTION users;
```

### Indexes & Foreign Keys

```sql
CREATE UNIQUE INDEX idx_email ON users (email);
CREATE INDEX idx_author ON posts (author_id);

ADD FOREIGN KEY fk_author ON posts (author_id) REFERENCES users (_id);
SHOW FOREIGN KEYS ON posts;
```

### Documents (DML)

```sql
INSERT INTO users VALUES ('{"_id":"u1","username":"alice","age":28}');

INSERT INTO posts BATCH VALUES
    ('{"_id":"p1","title":"Hello","author_id":"u1","likes":0}'),
    ('{"_id":"p2","title":"World","author_id":"u1","likes":5}');

SELECT * FROM users WHERE age >= 18 AND age <= 65 LIMIT 20 SKIP 40;
SELECT username, email FROM users WHERE status IN ('active','verified');
SELECT * FROM posts LOOKUP JOIN users ON posts.author_id = users._id;

COUNT  FROM posts WHERE likes > 0;
EXISTS IN users WHERE username = 'alice';

UPDATE posts
    SET title = 'Updated', updated_at = NOW()
    INCREMENT likes BY 1
    PUSH tags = 'trending'
    WHERE _id = 'p1';

UPDATE MANY users SET active = true WHERE age >= 18;

DELETE FROM posts WHERE likes = 0;
DELETE FROM logs  WHERE true;            -- explicit full delete
```

### Transactions

```sql
BEGIN TRANSACTION;
INSERT INTO orders   VALUES ('{"_id":"o1","total":99.5}');
INSERT INTO payments VALUES ('{"_id":"pay1","order_id":"o1"}');
COMMIT;      -- or ROLLBACK;
```

### Graph Definition & Build

```sql
CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
USE GRAPH social;

-- Nodes: collection → node type
MAP NODE User FROM users KEY _id PROPERTIES username, age;
MAP NODE Post FROM posts KEY _id PROPERTIES title, likes;

-- Edges, three flavors:
MAP EDGE FOLLOWS  FROM follows SOURCE from_id   AS User TARGET to_id AS User;   -- edge collection
MAP EDGE AUTHORED FROM posts   SOURCE author_id AS User TARGET _id   AS Post;   -- embedded FK
MAP EDGE LIKES    FROM posts UNWIND liked_by AS liker_id
                  SOURCE liker_id AS User TARGET _id AS Post;                    -- array UNWIND

BUILD GRAPH social;
GRAPH STATUS social;
GRAPH STATS  social;
GRAPH WAL STATUS social;
COMPACT GRAPH social;
SNAPSHOT GRAPH social INTO analytics_1;
```

### Traversal

```sql
TRAVERSE User('u1') OUT FOLLOWS DEPTH 1 LIMIT 50;   -- who u1 follows
TRAVERSE User('u1') IN  FOLLOWS DEPTH 1 LIMIT 50;   -- u1's followers
TRAVERSE User('u1') BOTH *      DEPTH 3 LIMIT 200;  -- 3-hop neighborhood, all edges

GET NODE User('u1');
EDGE EXISTS User('u1') -[FOLLOWS]-> User('u2');
```

### Algorithms

```sql
-- Fast, blocking (LiveGraph + shared lock)
RUN LOCK MutualFriends  ON social WITH user1='u1', user2='u2';
RUN LOCK MostConnected  ON social WITH metric='in', node_type='User' LIMIT 20;
RUN LOCK NetworkStats   ON social WITH mode='full';

-- Heavy, async (StaticGraph snapshot, background thread)
RUN JOB ConnectedComponents ON social;
RUN JOB CommunityDetection  ON social WITH max_iterations=10, members=true;
RUN JOB AllDistances        ON social WITH source='u1', max_hops=4;
RUN JOB AllDistances        ON social WITH all=true, max_hops=3;   -- all-pairs

JOB STATUS 'job_1';
JOB RESULT 'job_1';    -- blocks until done
SHOW JOBS ON social;
```

---

## 🧮 Graph Algorithms

Twelve algorithms ship with the engine, split into two execution tiers:

### 🔒 Lock Algorithms — instant answers on the live graph

| Algorithm | What it answers | Method | Complexity |
|---|---|---|---|
| `MutualFriends` | Common connections of two users | Two-pointer on sorted adjacency | O(deg₁ + deg₂) |
| `AreConnected` | Is there any path between A and B? | Bidirectional BFS | O(b^(d/2)) |
| `ShortestPath` | Hop-optimal path A → B | Bidirectional BFS + parents | O(b^(d/2)) |
| `FriendSuggestion` | "People you may know" | Friends-of-friends + Jaccard | O(deg × avg_deg) |
| `MostConnected` | Top-K influencers by degree | Cached-degree partial sort | O(V·log K) |
| `NetworkStats` | Live graph health snapshot | Atomic counters + single pass | O(1) … O(V+E) |
| `GetFriends` | Direct neighbors of a node | Adjacency read | O(limit) |
| `Neighborhood` | K-hop ego network | Bounded BFS | O(b^d) |

### ⚙️ Job Algorithms — heavy analytics on lock-free snapshots

| Algorithm | What it answers | Method | Complexity |
|---|---|---|---|
| `ConnectedComponents` | Islands in the network | Union-Find (DSU, path compression) | O(E·α(V)) |
| `AllDistances` | SSSP from one node — or **all-pairs**, sorted nearest→farthest | BFS (unweighted, so no Dijkstra needed) | O(V+E) / O(V·(V+E)) |
| `CommunityDetection` | Natural groups + **full member lists** per community | Label Propagation (LPA) | O(iter·(V+E)) |
| `PageRank` | Global influence ranking | Power iteration | O(iter·(V+E)) |

**Writing your own** is three steps — inherit `LockAlgorithm` or `JobAlgorithm`, implement `run()`, add the `.cpp` to `graph/CMakeLists.txt`. See [`graph/algorithms/ALGORITHM_TEAM_GUIDE.md`](graph/algorithms/ALGORITHM_TEAM_GUIDE.md) for the complete developer guide with API tables and templates.

```cpp
// C++ — run synchronously with a read lock
MutualFriends algo;
auto r = gm.runLock("social", algo, {"u1", "u2"});

// C++ — submit async job on a snapshot
ConnectedComponents cc;
auto handle = gm.submitJob("social", cc, {});
auto result = handle.result();   // blocks until finished
```

---

## 💻 CLI & Terminal Commands

### Unified command

```bash
nexoradb run          # start the database engine
nexoradb server       # start the FastAPI backend
nexoradb dashboard    # start the React admin dashboard
nexoradb cli          # interactive NexoraQL shell
nexoradb dev          # backend + dashboard together (dev mode)
```

### Individual entry points

```bash
nexoradb-server
nexoradb-dashboard
nexoradb-cli
```

### Typical developer workflow

**1.** Start backend and React dashboard together:

```bash
nexoradb dev
```

**2.** Or run them separately:

```bash
nexoradb run
nexoradb dashboard
```

Then open the interactive shell in another terminal:

```bash
nexoradb cli
nexoraql> SELECT * FROM users WHERE age > 18 LIMIT 5;
nexoraql> RUN LOCK NetworkStats ON social WITH mode='basic';
```

---

## 📁 Project Structure

```
nexoradb/
├── CMakeLists.txt              # root build (graph + python options)
├── vcpkg.json                  # rocksdb · fmt · pybind11
├── main.cpp                    # C++ engine self-test
├── test.py                     # Python binding self-test (19+ sections)
│
├── core/                       # DocEngine — document store on RocksDB
├── query/                      # QueryLayer — Condition / UpdateSpec / Evaluator
│
├── graph/                      # Graph engine
│   ├── GraphManager.*          #   orchestrator (build, snapshots, runLock/submitJob)
│   ├── LiveGraph.*             #   RAM projection, sorted adjacency
│   ├── StaticGraph.*           #   immutable snapshots, COO/CSR export
│   ├── GraphWAL.* GraphStorage.* GraphIdStore.*
│   └── algorithms/             #   12 algorithms + team guide
│       ├── AlgorithmBase.h     #   LockAlgorithm / JobAlgorithm / JobHandle
│       ├── MutualFriends.cpp · MostConnected.cpp · NetworkStats.cpp
│       ├── ConnectedComponents.cpp · AllDistances.cpp · CommunityDetection.cpp
│       └── ALGORITHM_TEAM_GUIDE.md
│
├── bindings/
│   └── nexoradb_bind.cpp       # pybind11 → nexoradb.so
│
├── nexoradb_python/            # Pythonic wrapper (NexoraDB / GraphDB, dict filters)
│
└── nexoraql/                   # ★ the query language
    ├── grammar/nexoraql.lark   #   Lark grammar (Earley)
    ├── parser.py               #   text → AST
    ├── ast_nodes.py            #   statement dataclasses
    ├── transformer.py          #   Lark tree → AST
    ├── semantic.py             #   validation + executor → nexoradb.so
    └── errors.py               #   Parse / Semantic / Execution errors
    │
├── app/                        # FastAPI backend
│   ├── api/v1/
│   │   ├── endpoints/          # 7 endpoint modules (query, documents, collections, graph, algorithms, apps, system)
│   │   └── models/             # Pydantic models (query, update, security, schema, graph, response)
│   ├── core/                   # config, dependencies, exceptions, native C++ bridge
│   └── services/               # engine_provider (singleton pattern)
│
├── docs/fastapi/               # API documentation (architecture, security, api_reference)
│
├── tests/                      # Unit and integration tests
│   ├── test_models/            # Pydantic model tests
│   ├── test_endpoints/         # API endpoint tests
│   └── test_integration/       # End-to-end integration tests
```

---

## ⚡ Design Principles

| Principle | Implementation |
|---|---|
| **One source of truth** | RocksDB holds everything; the graph is a rebuildable projection |
| **Reads never fight analytics** | Heavy jobs run on immutable `StaticGraph` snapshots — zero locks on OLTP |
| **Degrees are free** | `out_degree` / `in_degree` cached in 32-byte `NodeRecord`s — O(1), never counted |
| **Sorted adjacency** | Enables two-pointer set intersection (`MutualFriends` in O(deg₁+deg₂)) |
| **Right algorithm for the data** | Unweighted graph ⇒ BFS over Dijkstra; large networks ⇒ LPA over Louvain |
| **Crash safety** | Graph WAL with replay-on-startup; RocksDB WAL for documents |
| **GIL-aware bindings** | Python GIL released during C++ work, re-acquired before touching Python objects |

---

## 🧪 Testing

```bash
# C++ self-test (documents + graph, 18 sections)
./build/NexoraDB

# Python binding self-test (20 sections — CRUD, tx, joins, graph, snapshots)
python3 test.py
```

---

## 🗺 Roadmap

- [ ] String-dispatch `run_lock` / `submit_job` via a C++ `AlgorithmRegistry` (call any algorithm from Python by name)
- [ ] `MULTIPLY` / `SET MIN` / `SET MAX` update operators in the Python bindings
- [ ] Server-side `SORT BY` and projections in `DocEngine`
- [ ] Scheduled `REFRESH GRAPH ... EVERY n HOURS`
- [ ] Multi-join `LOOKUP JOIN` chains
- [ ] Replication & horizontal sharding
- [ ] Web-based NexoraQL playground in the dashboard

---

## 🤝 Contributing

Contributions are welcome!

1. **Fork** the repository and create a feature branch
2. New graph algorithm? Read [`ALGORITHM_TEAM_GUIDE.md`](graph/algorithms/ALGORITHM_TEAM_GUIDE.md) first — it has the full API, templates, and a delivery checklist
3. Keep `main.cpp` and `test.py` green
4. Open a pull request with a clear description

### Adding a New API Endpoint

1. Add the endpoint in `app/api/v1/endpoints/`
2. Add Pydantic models in `app/api/v1/models/`
3. Register the router in `app/main.py`
4. Add unit tests in `tests/test_endpoints/`
5. Document in `docs/fastapi/api_reference.md`
6. Update `README.md` endpoint table if needed

### Modifying Authentication

1. Update scopes in `app/api/v1/models/security.py`
2. Update `require_scope` calls in endpoints
3. Update `AVAILABLE_APP_SCOPES` tuple
4. Test with `tests/test_models/test_security.py`


---

## 📄 License

Released under the **MIT License** — see [LICENSE](LICENSE) for details.

---

<div align="center">

**NexoraDB** — *documents and graphs, finally speaking the same language.*

⭐ If this project helps you, a star means a lot!

</div>
