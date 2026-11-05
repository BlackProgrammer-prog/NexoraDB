# 0.5.3 CI registry and compatibility fixes

> Follow-up: these earlier tests allowed vcpkg binary caches. A subsequent
> uncached remote build exposed missing host `autoconf-archive` macros. Native
> and publication jobs now share the complete host prerequisite installer and
> an explicit preflight, and the debug job builds dependencies without binary
> caches. See [cold-build verification](ci-cold-build-v0.5.3.md).

The first remote native jobs checked out vcpkg commit
`7476f0d4e77d3333fbb249657df8251c28c4faae`, whose version database does not
contain the manifest's libsodium 1.0.22 override. Local validation used a newer
registry, so its successful builds did not reproduce that CI checkout. The
subsequent CMake compiler/Make errors followed the failed dependency install.

## Corrections

- Pin the vcpkg registry/tool checkout centrally in `cmake/vcpkg-revision.txt`:
  `cea592f4772491abdb7c483387a59ea89889f4be`.
- Native quality, publication and performance workflows all call
  `scripts/bootstrap_vcpkg.sh`. Cache keys include the registry revision.
- Before bootstrapping/configuring, `scripts/check_vcpkg_registry.py` verifies
  the checkout, manifest baseline, all four exact overrides and their port trees.
  Missing versions now fail at that explicit check. The original baseline is
  preserved, retaining the existing RocksDB and other dependency versions;
  libsodium remains at 1.0.22. The baseline is a dependency version floor, not
  a requirement to use an equally old registry checkout; see the
  [vcpkg versioning reference](https://learn.microsoft.com/en-us/vcpkg/users/versioning).
- Extend the existing TSan no-ASLR invocation to configure and build. During
  fresh dependency installation, libsodium's Autoconf compiler probe executes
  an instrumented binary. Without `setarch -R`, this was reproduced as
  `ThreadSanitizer: unexpected memory mapping` before product compilation.
  GoogleTest discovery can also execute instrumented binaries during build.
  ThreadSanitizer instrumentation and failure detection remain enabled.
- Require literal `True` for native graph/projection capabilities. A generic
  `Mock` previously made those checks truthy, bypassing the legacy transaction
  rejection and graph-startup failure tests. Dedicated native adapters already
  expose boolean `True` and retain their transaction support.
- Update the storage-budget integration fixture to enroll its temporary store
  offline and inspect it through an authenticated native session. The actual
  server factory still refuses uninitialized stores.
- Add regression tests for shared bootstrap use, missing overrides and TSan
  configuration/build invocation. Include them in the Python quality job.

## Local verification after the fixes

All build/configuration runs below used fresh output directories under
`build/ci-fix-053/`; vcpkg binary caches were allowed, as in CI. The new shared
bootstrap itself completed successfully and all overrides were verified.

| Check | Result |
|---|---|
| ASan, Python bindings OFF | Fresh configure and complete native/IAM build passed; **220/220 CTest groups passed**. |
| UBSan, Python bindings OFF | Fresh configure and complete native/IAM build passed; **220/220 CTest groups passed**, with halt-on-error enabled. |
| TSan full | Fresh configure built instrumented libsodium successfully; complete native/IAM build passed. **84/84 groups selected by the existing CI TSan preset filter passed**, with halt-on-error enabled. This is not a claim that every CTest case is in that filter. |
| Python quality command with native extension/IAM available | **181 passed**, including real IAM HTTP/TUI regressions. |
| Python quality command in a separate source copy without native binaries | **144 passed, 22 skipped**; native-only checks skipped as expected. |
| PyMongo security step in that copy | **4 passed**. |
| Registry/workflow regression tests | **3 passed**. |
| Three workflow YAML files and their shell bodies | Parsed; Bash syntax checks passed. |
| Bootstrap/checker lint and whitespace checks | Passed. |
| Product version consistency | Remains **0.5.3**. |

The source-copy test uses the existing dependency environment; it is not a new
claim about installing the latest possible dependencies. Logs are retained in
`build/ci-fix-053/validation/`. No commit, push, tag change, release upload or
remote CI rerun was performed. The maintainer must push these fixes and run CI
on the new commit; rerunning the old commit retains its old workflow and pins.

The wheel/hash in the earlier candidate report identifies the earlier staged
artifact. CI must rebuild a matching artifact from the corrected release source;
do not relabel the old wheel as containing these source changes.
