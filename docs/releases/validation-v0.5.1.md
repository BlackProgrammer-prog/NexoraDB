# Local release candidate validation — 0.5.1

Prepared in WSL on 2026-11-03 for a source push. No tag, public upload or remote
CI execution has been performed. The source branch is `develop`; the publication
workflow requires a reviewed release commit on `main` for publication.

## Candidate

- Version metadata: Python, CMake, vcpkg, frontend, lockfile and current READMEs
  agree on **0.5.1**.
- Incremental Release native rebuild from the current source, without ASan.
  Fresh dashboard build from the npm lockfile.
- Package staged into a new directory, isolated Python wheel build, GNU strip
  applied to the wheel staging copy, then auditwheel repair for CPython 3.10,
  Linux x86_64, manylinux_2_35 (glibc 2.35+).
- Wheel: `build/release-0.5.1-artifacts/wheelhouse/nexoradatabase-0.5.1-cp310-cp310-manylinux_2_35_x86_64.whl`.
- Size: **5,526,428 bytes**.
- SHA256: `d44e3e6f88a4e1381161a7e9586f21ad7642b8ba37542eda6b74b73fa2023acb`.
- Bundled native SHA256: `b19f694677564a77f00a2ca5874efad5d9514889cb91b36957abcb198a50d6c7`.
- Compared **87 Python/static/grammar files** in the wheel against current package
  source bytes. Frontend development test tooling is not included in the wheel.

## Validation

| Check | Result |
| --- | --- |
| CTest, including recovery crash matrix | 218 passed, zero failures |
| Python tests against the rebuilt packaged native module | 302 passed, 5 skipped, 18 subtests passed |
| Frontend development suite | 98 passed |
| Test TypeScript checks and ESLint | Passed |
| Dashboard production build and bundle budget | Passed |
| Chromium production smoke with API fixtures | Passed |
| npm lockfile portability and full audit | Passed; zero vulnerabilities |
| auditwheel repair/show and Twine check | Passed |
| Fresh-venv wheel install with binary extra | Passed |
| Installed metadata/native version and engine smoke | Passed; native reports Release 0.5.1 |
| Installed HTTP/setup/auth/dashboard/logo/static assets | Passed |
| Installed CLI and pip check | Passed |

Installed checks ran from `/tmp` with `PYTHONPATH` unset, against
`build/release-0.5.1-install-test`, so repository imports cannot mask missing wheel
files. The installed dashboard checks verify CSP, no-store HTML, immutable hashed
assets and the bundled logo. Dependency versions, logs, release manifest and
checksums are stored under `build/release-0.5.1-artifacts` (ignored by Git).

## Scope and publication

This is a patch to console behavior, security and dependency/release metadata.
No storage/index format change or new large-scale acceptance campaign is claimed.
The [0.5.0 limitations and migration precautions](v0.5.0.md) continue to apply.
The [frontend review](../FRONTEND_REVIEW_0.5.1.md) records remaining deployment
requirements, including gateway login rate limits and server-side pagination for
very large admin collection browsing.

The source commit includes dashboard build output, tests and release notes.
Local wheel binaries and environments remain outside Git; the release workflow
rebuilds and validates the wheel from source. A source push does not publish PyPI.
Review remote CI, merge to `main`, and use a matching `v0.5.1` release only when
ready to publish. Historical 0.5.0 artifacts and reports remain intact.
