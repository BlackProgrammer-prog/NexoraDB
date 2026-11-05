# Local validation: NexoraDB 0.5.3

Validated on 2026-11-05 in Ubuntu WSL, CPython 3.10 and Node.js 24.17.0.
The candidate includes uncommitted working-tree changes on base commit
`8df5d3972933d14a63c97eaf9c5399035cf8e04c`. The publication workflow must rebuild
from the eventual reviewed release commit; this report is not a remote CI run
or independent security approval.

## Checks completed

- Version synchronizer and expected-version checker: **0.5.3** across Python,
  CMake, package/manifests, frontend lockfile and current README references.
  The native module and offline executable also report 0.5.3.
- Native **Release + LTO** build: extension, offline IAM CLI, IAM tests/crash
  probe, document and graph tests built. Full CTest: **221/221 groups passed**,
  including native IAM and both crash/recovery matrices.
- Python final candidate run: **198 passed** across release metadata/imports,
  real native IAM HTTP/driver/NDJSON, IAM syntax and TUI, query workspace, admin
  auth/browser security, execution lifecycle, bounded execution, resource budgets,
  memory acceptance and TUI workspace/security suites. JUnit report is retained.
- An old resource-sizing fixture was updated to perform mandatory offline
  enrollment and inspect the authenticated native memory report; the factory's
  missing-enrollment refusal remains enforced.
- Native streaming's document envelope was corrected after the installed JOIN
  smoke exposed unframed rows. New real-native tests cover raw and projected
  NDJSON documents, including a document with its own `end` field.
- Frontend: **133 tests passed**, full ESLint, TypeScript production/test types,
  Vite production build and bundle budget passed. Entry bundle: 244588 bytes,
  77944 bytes gzip. No new browser automation run is claimed.
- New/changed IAM adapters, launcher and installed-package verifiers passed
  scoped Ruff checks. Both modified GitHub workflow YAML files parsed locally;
  scoped whitespace checks passed.
- Wheel built from a private staging copy; the developer's native library was
  not stripped. The wheel includes the stripped matching native extension,
  executable `native/nexora_iam`, thin `nexoradb-iam` launcher, Apache license
  and libsodium ISC notice. The executable's execute permission was checked.
- Auditwheel repair/show confirmed `manylinux_2_35_x86_64`; Twine check passed.
  Installed dependency compatibility (`pip check`) passed.
- Clean virtual-environment installation and all installed checks ran outside
  the checkout, from `/tmp`, without importing repository package sources:
  native CRUD/budgets/cancellation/sort/integrity/checkpoint/graph lifecycle;
  actual offline enrollment; owner login; collection grants/default deny;
  one-use user reset/replay refusal; legacy-constructor refusal; actual offline
  owner recovery and restart with the replacement password.
- Installed query/TUI smoke passed: native JOIN NDJSON, managed cursors,
  parameterized keyed mutation replay, request status, bundled dashboard/CSP,
  grammar, headless Textual workspace, real async monitoring transport and
  ordered service shutdown. All four installed entry points passed `--help`.
- **102 shipped Python, grammar and dashboard files** match the final source
  byte for byte. Stripped native binaries are validated by execution/version
  checks instead of comparing them to unstripped build output.

The initial clean dependency download encountered a DNS failure. Installation
was completed offline using cached wheels, with the resolved versions recorded
in `installed-dependencies.txt`; it is a clean install, not a claim to have
tested the latest possible dependency resolver outcome. A non-fatal
Starlette/httpx TestClient deprecation warning was observed. No new Python/npm
vulnerability audit, sanitizer/fuzz campaign, production-scale performance run
or independent penetration test was performed in this preparation.

## Candidate artifact

`nexoradatabase-0.5.3-cp310-cp310-manylinux_2_35_x86_64.whl`

Size: **9802328 bytes**. SHA-256:

```text
3f14b871dbacc2b3fab6e2b8baf5412b8460c793d3572af8636a1f016e827f69
```

Wheel and its raw/staged build, `SHA256SUMS`, `release-manifest.json`, dependency
freeze, JUnit XML and logs are under `build/release-0.5.3-artifacts/` (ignored
local output). The clean environment is `build/release-0.5.3-install-test/`.
Those artifacts describe this local tree; public CI generates its own artifact
and hash from the committed release source.

## Publication changes and remaining actions

The PyPI workflow now builds every IAM test/probe registered with CTest, stages
the offline executable in the wheel, supplies explicit matching native/CLI
paths to integration tests, and runs installed IAM recovery checks. The native
quality workflow also builds the new test targets and runs native IAM HTTP,
syntax and terminal regressions. The installed workspace verifier now uses
offline enrollment/native login instead of public registration/legacy JWTs.

Commit/push, reviewing and merging into `main`, the `v0.5.3` tag, remote CI and
GitHub Release/PyPI publication remain with the maintainer. No commit, push,
tag, release creation or public package upload was performed. No real user
database was adopted or recovered; all IAM exercises used temporary stores.

Review the [upgrade and limitations](v0.5.3.md) before release. The
[Graphy-based runtime roadmap](../architecture/CPP_RUNTIME_ROADMAP_FA.md)
records the Python/C++ boundary, analysis limitations and proposed follow-up
stages; that migration is not claimed as implemented in 0.5.3.
