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
