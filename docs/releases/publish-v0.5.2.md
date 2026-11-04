# Publication handoff: 0.5.2

## PR title

Release 0.5.2: unified query workspaces, streaming and TUI hardening

## PR body

Upgrade the web and terminal query workspaces with multiline editing, syntax
highlighting, server diagnostics, streaming and managed cursors. Extend supported
query-program delivery and native LOOKUP iteration with bounded resources,
execution receipts and explicit mutation replay protection. Improve terminal
monitoring, cancellation, credential handling and output safety.

Synchronize release metadata to 0.5.2 and ship a rebuilt dashboard. Add installed-wheel
query/TUI/monitoring verification to the publication workflow and include the Apache
license in distribution artifacts.

Validation: 414 Python tests and 18 subtests passed (5 skipped), 218 native tests,
127 frontend tests and 72 TUI compatibility tests passed. Frontend lint/types/build,
Chromium smoke, clean-wheel installation and installed native/query/TUI/monitoring
smokes passed. See docs/releases/validation-v0.5.2.md for artifact hashes and audit scope.
See docs/releases/v0.5.2.md for upgrade precautions and the unresolved optional
dnspython advisory on Python 3.10.

## GitHub Release / annotated tag

Tag: `v0.5.2`

Title: `NexoraDB 0.5.2 - Unified query workspaces and terminal hardening`

Use [v0.5.2.md](v0.5.2.md) as the release body (adjust relative documentation links
to repository links when pasting into GitHub). Do not omit the upgrade and known
security limitation sections.

## User publication steps

1. Review and commit the intended changes in your preferred groups. Include new
   untracked query/TUI modules, tests, documentation, release verifier, package
   LICENSE and regenerated dashboard assets; include deletion of obsolete assets.
   Do not include local build outputs, environments, database files or secrets.
2. Push the branch, review the PR and merge into `main`.
3. From the merged main checkout run `python3 scripts/check_release_version.py --expected 0.5.2`.
4. Create the annotated `v0.5.2` tag on that merged commit and push the tag.
5. Publish its GitHub Release using the title/body above. This triggers the PyPI
   workflow, which rebuilds and validates the Linux wheel. Check that workflow's
   success and published artifact; local preparation has not run remote CI.

No stage, commit, push, tag creation or publication has been performed automatically.
