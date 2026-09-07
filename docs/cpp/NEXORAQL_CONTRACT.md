# NexoraQL execution contract

NexoraQL remains SQL-like. `RENDER GRAPH` is unchanged. No Cypher engine is used.
This document describes the current source changes; rebuild the native extension
before deploying Python and C++ together. Older extensions do not expose the new
`DBResult.error_code` or `SchemaField.default_val` fields.

## Parameters

Parameters are values, never fragments of query text or identifiers. They are bound
while transforming the parse tree, not interpolated or parsed as additional SQL.

```python
client.execute(
    "SELECT username FROM users WHERE age >= $age LIMIT $limit;",
    {"age": 18, "limit": 100},
)
client.execute(
    "INSERT INTO users VALUES ($document);",
    {"document": {"username": "ali", "tags": ["database", "graph"]}},
)
```

HTTP accepts `{"query": "...", "parameters": {"age": 18}}`. The same parameter
argument is accepted by `parse`, `parse_one`, `Executor.execute_text`, and session
`execute`/`execute_one`. Missing parameters fail before any statement executes.

- Supported values: string, signed int64, finite float, bool, null, object, array.
- Object/array literals are supported for INSERT and UPDATE values; container
  predicate equality remains unsupported and is rejected, not coerced to a string.
- Example: `UPDATE users SET profile={"city":"Tehran","tags":[]} WHERE _id=$id;`
- Identifiers cannot be parameters. Driver convenience methods validate identifiers.
- LIMIT/SKIP require unsigned 32-bit integers, not strings, floats or booleans.
- `LIKE $pattern`, graph node IDs, traversal bounds and algorithm LIMIT/TOP accept
  typed parameters. LIKE retains the existing regex meaning, not SQL wildcard meaning.
- `parse_one` and `execute_one` reject multiple statements instead of discarding or
  executing extra statements silently.

## SQL-like graph queries

```sql
USE GRAPH social;
RENDER GRAPH social;
TRAVERSE User($user) OUT FOLLOWS DEPTH $depth LIMIT $limit;
EDGE EXISTS FROM User($source) TO User($target) TYPE FOLLOWS;
```

The previous `EDGE EXISTS User('a') -[FOLLOWS]-> User('b')` spelling remains a
compatibility alias for the same AST/executor; new examples use FROM/TO/TYPE.
Graph type checking, mapping filters/properties, and advanced graph pattern matching
are not implied by this syntax addition and remain separately tracked work.

## Read and JOIN semantics

- EQ/NEQ are aliases for =/!=. Keywords are case-insensitive.
- Exact string `_id` predicates use point lookup. Numeric/null constants are not
  coerced to a string identifier.
- Point reads honor SKIP. Only native `error_code=not_found` becomes an empty SELECT
  result; I/O/decode failures remain errors. An old native module without an error
  code fails closed rather than treating every error as NotFound.
- Projection preserves nested object/array shape. It is still performed in Python,
  not pushed down into BSON execution.
- Single LOOKUP JOIN uses the v2-capable FindMany target path for non-_id fields.
  It retains its existing first-match, inner-lookup behavior (one joined object).
  This is NOT a full relational many-to-many JOIN or a cross-collection snapshot.
- Source-qualified SELECT predicates/projection and qualified nested JOIN keys
  are normalized. JOIN-target predicates are explicitly unsupported.
- JOIN SKIP is applied after matching. Multiple LOOKUP JOINs remain unsupported.

## Documents, schemas and updates

- New inserts persist a generated `_id` inside the document when it is absent.
  Explicit IDs must be nonempty strings of at most 1 KiB.
- Insert defaults are applied before required/type/FK/index checks, in single,
  batch and explicit-transaction insert paths. Existing null is not missing.
- Native `SchemaField.default_val` is a JSON literal. Legacy unquoted string
  defaults remain accepted only when they are not valid JSON and the type is String.
- Numeric schema types are exact: Int32 enforces range; Float64 does not silently
  accept Int64. Null requires a Null schema type. Optional means missing is allowed,
  not automatic nullability. STRICT rejects unknown top-level fields except `_id`;
  an Object field does not recursively define the object's internal schema.
- UPDATE does not reapply insert defaults. `_id` is immutable. Invalid scalar/object
  traversal and array/numeric target mismatches are errors, not destructive coercions.
- Conflicting ancestor/descendant RENAME paths are rejected.
- Int64 comparisons and MIN/MAX avoid loss of precision through double conversion.
  Mixed-type index ordering/unique semantics remain an OPEN issue.

## Resource and failure boundaries

The parser caps input at 200000 characters, 256 statements, 20000 lexical tokens,
64 delimiter nesting levels and a bounded parse tree/value traversal. Parameters
have a 16 MiB serialized budget. Native compiled predicates have a 64-level/4096-node
budget. These are not hard wall-clock deadlines and do not solve regex ReDoS.

Application operation scopes are checked before executing a multi-statement request.
Transactions must finish inside one HTTP request. Unsupported transactional scans,
bulk updates/deletes and transactional graph projection remain rejected. Successful
earlier autocommit statements are not undone by later statement failure.

## Graph metadata and recovery status

New `.graphdef` metadata uses versioned JSON (format 2), with a legacy reader for the
old line-based format. Metadata is written to an exclusive temporary file, fsynced,
renamed, then the directory is fsynced on Linux. The previous file is never truncated
in place. The new durable-write helper deliberately has no Windows backend yet.

Graph names are restricted to SQL identifiers up to 128 characters to prevent path
traversal. Invalid definitions and failed native startup/load now propagate errors.
The external health route no longer returns success when engine/recovery fails.

**Automatic document-to-graph reconciliation is not complete.** Graph WAL alone
cannot detect a source commit that preceded a missed graph callback. Source-side
transactional events, projection checkpoints, consistent-snapshot generation builds,
atomic generation publication and crash-injection testing are still required.
