# Document Format Decision

Status: Accepted; BSON storage and online legacy migration implemented  
Target format: Nexora Document Envelope v1 with a BSON v1 payload  
API format: UTF-8 JSON and typed Python objects at public boundaries

## Decision

NexoraDB will not use JSON text as its canonical in-memory or on-disk document
representation. The canonical stored representation will be a small,
NexoraDB-versioned envelope containing a standards-compliant BSON v1 document.

JSON remains an interchange format for HTTP, CLI, NexoraQL, and compatibility
APIs. JSON is converted to BSON when it enters the storage mutation boundary.
The current compatibility execution path decodes a stored document once before
passing it to the existing query/update layer. Direct typed BSON query execution
is the next optimization and is intentionally not claimed by this milestone.

This hybrid provides BSON interoperability and type fidelity while retaining a
format-version boundary under NexoraDB's control for future compression,
checksums, or a new payload codec.

## Why this option

### JSON text as canonical storage is rejected

The current implementation stores JSON-like strings and repeatedly searches
and reparses them for field extraction. This has several problems:

- repeated O(document-size) parsing during validation, indexing, filtering,
  updates, foreign-key checks, and graph projection;
- ambiguous or incorrect behavior around escaping, nested containers,
  duplicate keys, malformed input, Unicode, and field names occurring inside
  string values;
- loss of exact numeric and binary types;
- expensive update operations that rebuild text;
- misleading C++ names such as `bson_document` for values that are not BSON.

JSON remains useful at system boundaries, but it is not suitable as the core
storage representation for the intended performance and correctness level.

### A fully custom binary document format is deferred

A custom format could eventually outperform BSON for a specific workload, but
designing it now would require independently solving validation, numeric
ordering, nested values, forward compatibility, tooling, fuzzing, language
bindings, and corruption handling. That cost and correctness risk are not
justified before representative BSON workloads have been measured.

### BSON inside a versioned envelope is selected

BSON supplies typed scalars, nested documents, arrays, binary data, explicit
lengths, and a mature ecosystem. The NexoraDB envelope provides safe format
detection and a future codec migration point without changing RocksDB keys or
public APIs.

## Physical layout

Every new document value uses this layout:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 bytes | ASCII magic `NXD1` |
| 4 | 1 byte | Payload codec (`1` = BSON v1) |
| 5 | 1 byte | Flags; initially zero |
| 6 | 2 bytes | Reserved; must be zero |
| 8 | variable | Complete BSON document including its BSON length and terminator |

All integer fields introduced by the envelope will use little-endian encoding.
The BSON payload follows the BSON specification. RocksDB already protects data
blocks with checksums, so v1 does not duplicate every document with another
value-level checksum. A future flag may add one for replication or transport.

Legacy values are accepted only until the database-wide migration marker is
published. An unknown `NXD` envelope is always rejected as corruption instead
of being guessed as JSON.

## Implemented compatibility behavior

- Existing C++ and Python method names, JSON arguments, query objects, and JSON
  result strings are unchanged.
- `MutationBuilder` is the only document write boundary and encodes every new
  insert and replacement to BSON before staging the RocksDB transaction.
- Point reads, scans, pagination, joins, graph collection iteration, online
  index builds, and transactional reads accept both legacy JSON and BSON v1.
- The idle migration worker waits for a foreground-idle window, converts at
  most 1,000 documents or 4 MiB per transaction, and yields between chunks.
- The migration cursor and converted documents commit atomically. Conversion
  is idempotent and resumes after restart. A final database-wide format marker
  stops future legacy scans.
- Operators and tests may run one deterministic chunk through
  `MigrateLegacyDocuments` or Python `migrate_legacy_documents`.

## Required internal abstractions

The storage engine should expose these internal types rather than passing
ambiguous `std::string` values throughout the core:

```cpp
enum class DocumentCodec : std::uint8_t {
    LegacyJson = 0,
    BsonV1 = 1,
};

class DocumentView;    // immutable, non-owning, validated typed view
class DocumentBuffer;  // owning encoded document
class DocumentBuilder; // mutation/copy-on-write builder
```

`DocumentView` must parse/validate once per operation. Compiled field paths
should resolve directly against that view. Schema validation, index tuple
extraction, residual predicates, foreign keys, joins, and graph projections
must share the same view instead of independently parsing the document.

The first production implementation should use a mature BSON implementation
for validation and writing. A zero-copy read-path wrapper may be optimized only
after correctness, fuzz testing, and benchmark parity are established.

## Public API naming

Public interfaces must state the accepted or returned representation:

```cpp
InsertOneJson(collection, std::string_view utf8_json)
InsertOneBson(collection, std::span<const std::byte> bson)
FindByIdJson(collection, id)
FindByIdBson(collection, id)
```

Existing `InsertOne`, `InsertMany`, and transaction methods remain JSON
compatibility APIs for the 0.x series. Their parameters and documentation must
be renamed from `bson_document` to `json_document`; they can delegate to the
new JSON boundary converter. Removing these compatibility methods requires a
separately announced breaking release.

Python bindings should prefer typed Python mappings and native `bytes`:

```python
db.insert_one("users", {"name": "Alice", "age": 28})
db.insert_one_bson("users", bson_bytes)
db.find_by_id("users", document_id)       # returns a mapping
db.find_by_id_bson("users", document_id)  # returns bytes
```

HTTP and NexoraQL continue to use JSON-compatible values. Arbitrary binary data
must use a typed binary API or an explicitly documented Extended JSON form;
raw bytes must never be smuggled through an ordinary JSON string.

## Type semantics

- Missing and BSON null are distinct values.
- Empty string is distinct from both missing and null.
- Int32, Int64, Double, Decimal128, Boolean, DateTime, ObjectId, String, Binary,
  Array, and Document retain their BSON type.
- Numeric comparison may compare numeric types by value, but storage and
  round-trip APIs must preserve the original type.
- NaN ordering and equality must be defined explicitly before it is indexable.
- Duplicate field names are rejected at ingestion even if a BSON parser can
  technically iterate over them.
- Field names and BSON strings must contain valid UTF-8. Arbitrary bytes belong
  to the BSON Binary type.
- Arrays preserve order. Document field order is preserved for round trips but
  must not affect ordinary document equality unless explicitly requested.
- `_id` remains mandatory internally and is generated when absent. During the
  first migration, existing string `_id` behavior remains unchanged.

The index codec must receive typed values from `DocumentView`; it must never
redetect BSON types from rendered strings.

## Limits

The initial compatibility profile is:

- maximum BSON document size: 16 MiB, including the BSON document itself but
  excluding the 8-byte Nexora envelope;
- maximum nesting depth: 100 document/array levels;
- maximum `_id` encoded size: 1 KiB;
- maximum compiled field-path components: 100;
- maximum field-name length: 1 KiB of UTF-8;
- invalid BSON, invalid UTF-8, duplicate keys, excessive depth, or oversized
  values are rejected before a transaction performs any mutation.

Limits must be checked iteratively where possible. A recursive validator must
not rely on the process call stack for hostile documents.

These defaults intentionally match MongoDB's document-size and nesting profile
where practical. NexoraDB may make limits configurable later, but persisted
collection metadata must record non-default values so all processes enforce the
same rules.

## Migration metadata

Because all document write paths switched together, the initial implementation
uses one database-wide state instead of carrying permanent per-collection
mixed-format complexity:

```text
meta:format:documents             -> "1" after the full scan commits
meta:migration:documents:v1       -> last fully committed document key
```

The engine never infers steady-state format from a sample document. During the
bounded migration it identifies each value by its explicit envelope; legacy
JSON is parsed and validated before conversion.

## Migration from existing data

Migration is online, resumable, bounded, and idempotent:

1. If the database-wide completion marker is absent, enter resumable mixed-read
   mode.
2. New inserts and updates are written as BSON v1 immediately.
3. Readers accept both JSON v0 and BSON v1 while this state is active.
4. Scan legacy documents in bounded chunks (default: at most 1,000 documents
   or 4 MiB of input per transaction).
5. For every document, acquire the normal document transaction lock, parse and
   validate JSON strictly, encode BSON once, and checkpoint the migration cursor
   in the same transaction. Index entries are unchanged because migration does
   not change logical values.
6. Concurrent updates win through normal RocksDB conflict detection; a
   conflicted migration item is retried from a fresh value.
7. At the end of the bounded scan, atomically publish the BSON completion marker.
8. On crash, resume from the committed cursor. On validation failure, retain
   mixed-mode readability and expose the exact document key and error; never
   claim migration success.

A dry-run command must report invalid JSON, duplicate keys, depth violations,
unsupported numeric values, and projected storage growth without writing.
Before migration, operators should create a RocksDB checkpoint. Rollback means
restoring that checkpoint; the engine must not promise an unsafe in-place
reverse conversion.

## Performance requirements

The BSON transition is accepted only if benchmarks demonstrate:

- one validation/parse per mutation, not one parse per accessed field;
- indexed query cost proportional to candidates;
- point-read BSON path without JSON serialization;
- JSON serialization excluded from native storage-engine benchmarks unless it
  is the workload under test;
- batch conversion with bounded peak memory;
- p50, p95, and p99 results for 1 KiB, 16 KiB, 256 KiB, and near-limit documents;
- separate measurements for flat, nested, array-heavy, and binary-heavy data;
- graph projection built from typed views without JSON round trips.

Benchmarks must compare legacy JSON, BSON input/output, JSON boundary
conversion, index extraction, updates, and graph snapshot construction.

## Acceptance tests

- malformed JSON and malformed BSON are rejected before any RocksDB write;
- escaped strings and field-name text inside values cannot confuse lookup;
- null, missing, empty string, Unicode, Binary, Decimal128, and nested arrays
  round-trip without type loss;
- duplicate keys, a 101-level document, and a document larger than 16 MiB fail;
- the maximum valid size and depth succeed;
- migration resumes after injected crashes at every chunk boundary;
- reads and writes remain correct during mixed-mode migration;
- index results before and after migration equal a full-scan reference;
- no document/index/counter mutation is partially committed;
- old JSON API behavior remains compatible for valid existing inputs;
- fuzzers cover envelope decoding, BSON validation, field paths, conversion,
  and migration resume state.

## Graph-engine consequence

The graph engine should consume typed `DocumentView` values directly and intern
node/edge IDs and property keys. Its adjacency and analytics structures remain
purpose-built binary structures; BSON is the source-document representation,
not the in-memory graph representation. Competing with graph-native databases
depends on adjacency layout, snapshots, algorithms, and concurrency—not on
reusing the document serialization as the graph execution format.
