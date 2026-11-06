# 0.5.3 cold-build host prerequisites

The next remote logs reached the pinned registry successfully. The ASan and
TSan logs then explicitly failed in `vcpkg_run_autoreconf`: the runner lacked
the `autoconf-archive` macro package. Installing `autoconf` and `automake` alone
does not supply `AX_CHECK_COMPILE_FLAG`. The previous local host already had
the macro package and permitted binary caches; those checks did not validate
the runner's source-build prerequisites.

The debug/UBSan excerpts end at the outer dependency-install failure without
the underlying port error. The workflows now preserve those inner logs instead
of inferring a cause from the subsequent compiler/Make diagnostics. Downloading
CMake 4.3.2 is vcpkg obtaining its own build tool, not itself an error.

## Changes

- `scripts/native_prerequisites.sh --install` installs a shared Ubuntu host-tool
  list, including `autoconf-archive`, in both native-quality and PyPI workflows.
  It also verifies the required executables and actual archive macro files.
  `--check` performs the same preflight without installing anything. The
  self-hosted performance workflow runs that check and does not modify the host.
- Debug CI exports `VCPKG_BINARY_SOURCES=clear`, so source dependency builds are
  exercised even when another job could restore a successful archive. Downloads
  of source tarballs remain eligible for caching.
- Dependency build concurrency is explicitly limited to two; publishing uses
  GCC/G++ 11 consistently with native quality.
- Both native-quality and failed wheel builds retain the manifest install log
  and per-port logs, including inner CMake/Autoconf errors.
- Regression tests simulate executable Autoconf tools with missing archive
  macros; preflight must reject that environment. The matching complete macro
  environment passes. Workflow wiring and the uncached debug gate are checked.

## Verification

The cold run used Ubuntu 22.04 under WSL, GCC/G++ 11, **outer CMake 3.31.6** and
vcpkg's **CMake 4.3.2**. The latter is confirmed in the dependency CMake cache.
It used newly created install, buildtrees and packages directories under
`build/ci-cold-053/`, with `VCPKG_BINARY_SOURCES=clear` and concurrency two.
No precompiled vcpkg dependency archives were restored. Existing source tarballs
were reused after the port's usual hash checks. The local preflight used vcpkg's
downloaded Ninja executable in PATH; CI installs Ninja through apt.

- All **16 dependency packages built from source**, including libsodium 1.0.22
  and both Debug/Release RocksDB libraries. Dependency installation completed in
  approximately 8.2 minutes; project configure/generate succeeded.
- The native document/graph tests, IAM CLI/tests/crash probe and Python extension
  built successfully against those freshly compiled dependencies.
- Full CTest on this build passed **221/221 groups** after correcting the
  contention-test handling described below.
- Native IAM HTTP and prerequisite/registry regressions passed **19 tests**
  against the newly built Debug extension and CLI.
- After restricting retries to pre-commit counter-staging failures, the counter
  and forced-timeout/rollback tests passed **20 repetitions each**.
- Workflow YAML and Bash body syntax, script syntax, scoped Ruff and release
  version consistency checks passed. Product version remains 0.5.3.

The first full run also exposed an intermittent counter-concurrency test
failure: a mutation failed, while the stored count and actual document count
both remained 201. The old test discarded the failure reason and assumed every
contended mutation would succeed despite a one-second lock timeout. Its counter
assertions are unchanged. It now permits bounded retries **only for explicit
pre-commit counter-staging transaction conflicts/timeouts/deadlocks** and reports
any remaining error. Commit, I/O and unknown failures are never replayed. The
separate forced-lock-timeout/rollback contract remains tested. No production
database code or runtime timeout was changed for this fixture correction.

Logs, including the original failed full run and counter repetition, are under
`build/ci-cold-053/`. These are local checks, not a claim that a new GitHub run
has passed. No commit, push, tag or publication was performed. Push the shared
script, regression tests and workflow changes together before running CI on
the new commit.
