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
