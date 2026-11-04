# Local validation: NexoraDB 0.5.2

Validated on 2026-11-04 in Ubuntu WSL, CPython 3.10 and Node.js 24.17.0.
This candidate includes uncommitted working-tree changes. The publication workflow
rebuilds from the eventual release commit; these hashes identify the local candidate only.

- Version consistency: 0.5.2 across Python, CMake, manifests and release references.
- Python: 414 passed, 5 skipped, 18 subtests passed.
- Native Release build and CTest: 218 passed.
- Frontend: lint, types, 127 tests, production build, bundle budget and Chromium smoke passed.
- TUI compatibility: 72 tests passed with Textual 0.86.3 and Socket.IO 5.11.0.
- CLI lint and release-verifier lint passed.
- Clean wheel install outside the checkout: native CRUD, budgets, cancellation,
  sorting, integrity, graph lifecycle and cleanup passed.
- Installed query delivery: native JOIN stream, cursor, mutation replay/receipt,
  headless Textual workspace, actual async monitoring transport and shutdown passed.
- All three installed CLI entry points passed their help smoke checks; pip check passed.
- Twine and auditwheel checks passed; Apache license included.
- 97 shipped Python, grammar and dashboard files match the working tree byte for byte.
  Native binaries are stripped/repaired during packaging and are validated by execution.
- npm audit: zero findings. Clean default Python installation: no known third-party
  dependency findings after bootstrap-tool upgrades. The unpublished project version
  is skipped by the advisory service. Optional development dnspython remains affected;
  see [release notes](v0.5.2.md#known-security-limitation).

## Candidate artifact

`nexoradatabase-0.5.2-cp310-cp310-manylinux_2_35_x86_64.whl`

Size: 5699900 bytes. SHA-256:

```text
7b773aa1eea788f3b0cdcc196d7313b084a2d4f8af7e4141057642c0741c2f23
```

Wheel, source archive, logs, SHA256SUMS and release-manifest.json are retained under
`build/release-0.5.2-artifacts/` (local ignored build output).
The source archive is for review; the current release workflow publishes the repaired Linux wheel.

No new million-row benchmark or independent security certification was performed.
No commit, push, tag, remote CI run or public upload was performed during preparation.
