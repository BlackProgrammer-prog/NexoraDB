# Native IAM: operation, migration and security boundaries

Native login, sessions, collection authorization, administrative identity/role
management, scoped service credentials, password changes/resets and offline owner
recovery are implemented. This replaces the earlier phase-one-only lockout.
Independent production security approval is a separate release gate.

## Authority and interfaces

| Component | Implementation / behavior |
|---|---|
| Identity enrollment and recovery | `core/security/NativeIam.*`, `scripts/iam.cpp`; offline RocksDB process lock, synchronous WAL-enabled atomic writes, owner-selected username, immutable random root/domain IDs, eight one-time recovery factors. |
| Authentication and authorization | `core/security/SessionAuthority.*`; Argon2id verification, opaque native sessions, current-account checks, generation-based revocation, role inheritance/cycle rejection, exact collection grants, rate limits and durable security audit. |
| Protected data gateway | `core/security/SecureDatabase.*`; privately owns `DocEngine` and exposes a native operation whitelist. No raw engine, RocksDB pointer, privileged context or credential verifier is returned. Unsupported operations deny. |
| Python bindings | `bindings/secure_bindings.cpp`; typed requests and native exceptions. Caller-supplied usernames/roles are never an authenticated security context. The legacy `DocEngine` constructor still refuses enrolled stores. |
| Graph gateway | Native `GraphGateway` in `bindings/secure_bindings.cpp`; operations require an interactive superuser. Initialization/drain and committed-data projection refresh are fixed internal maintenance tasks. Mutable raw graph handles are never returned. |
| HTTP adapters | `nexoradb_admin/native_iam.py`, `iam_routes.py`, `app.py`; tokens are bound per request and every native operation revalidates them. Server factories require native IAM; no automatic legacy fallback or HTTP owner enrollment. |
| Python driver | `NexoraDBClient.connect(...)`, `change_password`, `logout`, `complete_password_reset`; existing token construction also accepts native service credentials. Verified HTTPS is required remotely. |
| React | Local enrollment guidance, arbitrary owner username, real one-use reset form, account password change and `/security` users/grants/roles/audit controls. Logout requests native revocation. Tokens remain in tab memory. |
| Textual TUI | Local setup guidance, login/reset tabs, account/access panel, explicit collection grants for application tokens, password changes and native logout. Credential forms do not use the query workspace/history. |

Legacy embedded `DocEngine` remains available for trusted local/test use on
**unenrolled** databases. It is not an authentication boundary. Enrolled stores
reject that constructor, including direct Python attempts. Server startup/access
to an unenrolled store requires explicit local setup or adoption. Python code
with unrestricted host/filesystem/process control is not sandboxed by IAM.

## Setup and migration

Installed 0.5.3 wheels include the native offline executable. Use `nexoradb-iam
--version`, `nexoradb-iam init /absolute/path`, `nexoradb-iam adopt /absolute/path`
or `nexoradb-iam recover /absolute/path`; the launcher directly executes the
bundled binary and does not receive or hash credentials itself. Source-build
equivalents below use `build/debug/nexora_iam`. Set the server's database/graph
configuration to the same intended absolute paths before starting it.

Build the native executable and matching Python module:

```sh
cmake --preset debug
cmake --build build/debug --target nexora_iam nexoradb_py nexora_iam_tests nexora_iam_crash_probe -j 3
export NEXORADB_NATIVE_MODULE_PATH="$PWD/nexoradb.cpython-310-x86_64-linux-gnu.so"
```

Use the same matching binary for server, driver package and tests. The Python
package honors the configured native path before loading a bundled extension;
loading two different extension versions into one interpreter is refused.
Production wheel/release packaging must include the reviewed matching binary.

For a new, empty store:

```sh
umask 077
build/debug/nexora_iam init /private/nexoradb
build/debug/nexora_iam status /private/nexoradb
```

For existing data, first stop the service and make a verified backup:

```sh
build/debug/nexora_iam adopt /private/existing-nexoradb
```

Adoption requires typing `ADOPT EXISTING DATA`. If a legacy `root` account
exists, its password must be verified before migration. Supported legacy
credentials are the existing Argon2id 64-MiB profiles (the old Python default
three-pass/four-lane profile or the native two-pass/one-lane profile); unknown or
bcrypt credentials are refused rather than bypassed. Unsupported old storage
formats must be migrated first. An inconsistent legacy identity store without
its owner is refused.

The operator chooses the new owner username/password. The one atomic transition
installs the native identity and reader fences, deletes legacy user/token
documents, records adoption metadata and revokes legacy credentials. User data
remains in place. Existing graph directories are supplied through the normal
trusted server configuration. Rehearse adoption and restore on a copy before
applying it to production data. No real user database was adopted during this
implementation work.

New database directories receive owner-only permissions; protect existing
directories, ancestors, Windows ACLs, logs and backups operationally. Status
opens the offline store and can create an empty RocksDB for a new path. Verify
the path. Do not put passwords or factors into argv, environment variables,
NexoraQL text, URLs, CI output or service logs.

## Credential and session behavior

Passwords are hashed by pinned libsodium 1.0.22 using Argon2id 1.3, 64 MiB,
two passes, one lane and random salts. Native verification rejects malformed
PHC strings and unsupported work factors before running a KDF. Passwords are
15..1024 bytes without NUL, never trimmed or normalized; web forms additionally
require 15 characters. Usernames are case-sensitive ASCII `[A-Za-z0-9_.-]`,
1..64 bytes. `root` is an allowed owner-selected name, not a reserved identity.

The protocol is a TLS-protected password exchange with native verification,
followed by opaque 256-bit bearer credentials. Argon2 verifiers are not reused as
SCRAM credentials. Drivers/browser endpoints refuse insecure remote HTTP;
server HTTP and monitoring adapters also reject insecure remote credential
transport. Loopback HTTP is for local development. Terminate TLS at the service
or a trusted proxy, restrict proxy forwarding trust, and do not expose an
unprotected upstream listener. No credential body is echoed in validation errors.

Interactive sessions have 15-minute idle and 8-hour absolute expiration, with
at most 4096 live sessions per native authority. Only token digests, immutable
principal IDs, credential generations and native time state are retained.
Interactive sessions are process-local and disappear on restart. Explicit
logout revokes the particular session; password change/reset, disablement,
rename and direct-access updates invalidate the affected account's sessions and
delegated credentials. Custom-role edits invalidate ordinary-user credentials
so cached rows cannot survive revoked inherited privileges.

Security writers stop admission of new readers while awaiting already-admitted
operations. Revocation is not instantaneous cancellation of a running operation:
it becomes effective after admitted native work finishes. New operations check
current state, not cached client role flags. Native execution has a default
30-second deadline; an explicit execution scope can be up to 300 seconds.
Readers encountering a pending security update receive a retryable capacity
error. This avoids indefinite writer starvation from fresh reads.

Authentication admits one expensive operation at a time per authority and
serializes KDF memory across the process. Five failed attempts block an account
or source for 60 seconds. The attempt table is bounded at 4096 entries and prunes
inactive entries after ten minutes. Missing users use a dummy verifier. These
are local-process protections, not distributed IP reputation or federation.

## Authorization model

New users start with zero roles/grants and no implicit data access.
Authorized superusers create/disable/enable/rename accounts and assign exact
collection grants or custom roles. Only the protected owner can newly assign
the built-in `superuser` role. The owner cannot be disabled or lose superuser.
The built-in role cannot be redefined or inherited by a custom role.

```json
{"orders": ["read"], "invoices": ["read", "write", "manage"]}
```

`read` permits collection data/metadata reads, `write` permits document mutation,
and `manage` permits collection/schema/index/FK changes. Manage does not imply
read/write. Grants are exact, case-sensitive collection names; no wildcard or
explicit-deny precedence is implied. Role inheritance is acyclic, up to 128 roles
and 16 parent/assigned roles per record. The ordinary-user limit is 1024.

Native joins authorize every source/target collection. FK-constrained writes
also require read access to referenced and incoming-reference collections, so
constraint validation cannot probe otherwise hidden data. A gateway schema gate
holds this metadata stable through checking and mutation. Collection discovery
filters out resources without grants and all reserved security collections.
Session-owned pagination tokens cannot be borrowed by another session or have
their join authorization removed. Even already-buffered stream rows revalidate
the native session before delivery.

Transactions are session/database/thread-bound, expire after 30 seconds, and
are limited to 64 active handles. A policy-epoch change prevents commit and
rolls back. Schema changes reject while transactions are active rather than
deadlocking their commit. HTTP programs keep native transactions on their
creating worker and close them in finally paths; embedded callers must honor
the existing same-thread handle lifetime contract and drop abandoned handles.

Graph data, traversal, algorithms and snapshots currently require an interactive
superuser. Ordinary users and service credentials cannot obtain graph/topology
access. Server-owned graph gateways refresh affected mapped graphs from committed
documents, including transaction commits; caller-supplied old/new document hooks
do not mutate graph state. The conservative refresh rebuilds affected graphs:
measure its write cost on mapped collections before deployment. Document commit
and graph publication are not a distributed atomic transaction. Refresh failure
reports an unknown/partial outcome requiring reconciliation, not a false rollback.

**Fine-grained row/document/field/property policies are not implemented.**
Collection read exposes the collection's records and fields. Security APIs reject
unknown policy fields; do not claim field masking, graph tenant isolation or
vector noninterference. Vector/time-series operations absent from the gateway
are denied. Native resource UUIDs/rename-following grants are not delivered.

Permission-only NexoraQL administration supports `CREATE OR REPLACE ROLE ...
GRANTS {...} INHERITS [...]`, `ALTER USER '<immutable-id>' SET ROLES [...]`,
`ALTER USER '<immutable-id>' SET GRANTS {...}`, `SHOW SECURITY USERS` and
`SHOW SECURITY ROLES`. These require native IAM and an interactive superuser;
only the protected owner can grant SuperUser. Definitions/role lists/direct
grant maps are replacements, not additive grants. Role updates invalidate
ordinary-user credentials; user updates invalidate the target's credentials.
They cannot run inside document transactions or through service-token scopes.

Passwords, user creation and reset/recovery factors use dedicated forms/APIs,
never password literals in query history. `POST /security/users` accepts optional
`roles` and `grants` with the initial password and commits the entire new account
in one native security batch; omitted permissions default to empty.
See the [Persian user and recovery guide](users-and-recovery-fa.md) and runnable
role definitions in `examples/presentation/social_demo.nql`.

## Password reset and recovery

Authenticated users change passwords with the current password. A superuser can
issue a non-owner reset token, displayed once, expiring in 15 minutes. Issuance
revokes existing account credentials immediately and blocks password login until
completion. `/auth/reset-password` consumes the token atomically with the new
credential, increments its generation and prevents replay. Deliver the token
through an organizationally verified channel; there is no username/email-only
public reset endpoint or pretend email-delivery workflow.

Emergency owner recovery requires stopping the service, obtaining the database
process lock and presenting an unused enrolled recovery factor:

```sh
build/debug/nexora_iam recover /private/nexoradb
```

The CLI reads factor/new password through hidden terminal input. One durable
batch changes the password, consumes the factor, increments the security epoch,
revokes every persistent service credential and pending reset, and records a
recovery audit event. Restart also discards all interactive sessions. Used factors
cannot be replayed. There is no `--force`, universal secret or remote owner reset.
If factors are lost, host access alone does not establish a supported recovery
procedure. After signing in as owner, `/security/recovery/rotate` re-verifies the
current password and enrolls eight replacement factors, displayed once.

## Service credentials and replay safety

Service credentials are opaque random tokens, stored only as digests. Issuance
requires an interactive superuser, explicit collection grants and API scopes.
Lifetimes are 300 seconds to one year, default one day; no perpetual token is
issued. Native evaluation intersects the credential's scopes/grants with the
issuer's current status/generation/permissions on every call. Revocation survives
restart. Lists return metadata, not recoverable plaintext credentials. Tokens
cannot access console administration or root recovery.

An interactive driver connection owns its native session and logs out when
closed; closing a client created with an externally supplied service token only
closes its connection pool. For example:

```python
from getpass import getpass
from nexoradb.api.driver import NexoraDBClient

with NexoraDBClient.connect(
    url="https://database.example",
    username=input("Username: "),
    password=getpass("Password: "),
) as db:
    result = db.execute("SELECT * FROM orders LIMIT 10;")
```

Idempotency keys remain tied to stable native principals, with the credential
identifier included in the request fingerprint. A different credential cannot
read an earlier cached result or automatically rerun the same keyed mutation;
it receives a conflict and must reconcile. Native request-status inspection
returns status only, not old data/receipts. Journals remain separate from RocksDB:
ambiguous outcomes are not a claim of distributed exactly-once transactions.

## Durable state and audit

`sec:state:v1`, `meta:iam:required`, the component manifest's `iam:1` and the
generation-three reader marker preserve the bootstrap fence. Missing, empty,
unsupported or contradictory identity metadata locks the store; corruption does
not reopen enrollment. New credential/security mutations use synchronous WAL
batches regardless of document durability settings. A write error latches an
uncertain outcome and rejects further authority work until offline inspection.

Audit events at `sec:audit:<sequence>` are committed with security state and chain
BLAKE2b digests over previous-event hashes. Login failures/success, logout,
credential changes, resets, recovery and role/user/token changes are recorded
without passwords or bearer/recovery/reset factors. Reads expose the latest
1000 events to superusers. This is not an externally anchored immutable audit
service; retention/export and external anchoring still need deployment work.

## Validation and remaining release gates

Validation in this workspace: 115 Python tests passed across native HTTP/driver,
TUI, browser/transport, authorization, execution lifecycle and bounded execution
suites; 130 frontend tests passed and TypeScript/build checks passed. The full
native CTest run passed 221 groups; after the final IAM durability hardening,
the focused IAM rerun passed all three groups (30 native IAM cases, nine crash/
WAL cases and terminal/CLI checks). These results are local verification, not
independent production certification.

The role/UI follow-up adds tested native enforcement for atomic creation with
initial roles/grants, protected-owner-only SuperUser promotion, permission-only
NexoraQL, ordinary/service-token rejection and React/TUI administration visibility.
The follow-up run passed 96 Python tests (native HTTP, syntax/presentation,
query workspace, auth and TUI), all 133 frontend tests, TypeScript and test-type
checks. The IAM CTest groups passed again after extending native account creation.

Tests include native bootstrap corruption/concurrency and WAL/SIGKILL cases;
native sessions, expiry, revocation, role cycles, transaction ownership and
stale-policy rollback; offline recovery/replay and migration proof; real native
HTTP, driver login/transaction/logout, service-token revocation, validation
redaction and cached-row revocation; React auth/reset forms and TUI privacy.

```sh
ctest --test-dir build/debug --output-on-failure -j 4
NEXORADB_NATIVE_MODULE_PATH="$PWD/nexoradb.cpython-310-x86_64-linux-gnu.so" \
  PYTHONPATH="$PWD/NexoraDB/src" NexoraDB/.venv/bin/python -m pytest \
  tests/test_native_iam_http.py tests/test_iam_tui.py tests/test_tui_security.py -q
cd frontend
npm run test:run
npm run build
```

Production release still requires independent security review, fault testing of
all new mutation workflows, sanitizer/fuzz coverage, deployment-sized KDF/query/
graph-refresh benchmarks, release-binary packaging and migration/restore drills.
RocksDB strict recovery can leave a deliberately truncated WAL locked; do not
clear fences or salvage it into a new bootstrap. Physical database replacement,
complete metadata rewriting, clock rollback, or restoring an old full backup
cannot be authenticated by local JSON fences alone. There is no external backup
rollback anchor yet; isolate backups, restore under authorized offline procedure,
invalidate restored service credentials and rotate credentials/factors before
exposing a restored instance. IAM-aware online restore remains unavailable.
No Windows build, independent penetration test or production approval is claimed.
