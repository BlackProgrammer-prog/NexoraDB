# Publishing 0.3.0 from main

This checklist does not publish anything automatically from a local checkout.

1. Review the release changes on `develop`, commit them, push that branch,
   and open a pull request into `main`. Require the native quality checks to
   pass, including the separate sanitizer jobs. Do not tag an unreviewed or
   failing revision. Configure required checks in branch protection as needed.
2. Merge the PR. Confirm the default branch is `main` and contains
   `.github/workflows/performance.yml` and `publish-pypi.yml`.
   The Actions page then lists **Fixed-host performance history**.
3. Run **Publish NexoraDB to PyPI** with `publish=false` on `main`
   first (use the display name in the workflow if it changes). Inspect the
   build/test, repaired-wheel, and isolated-install results and artifacts.
4. For performance history, start the WSL runner and leave it running; its
   labels must include `nexora-perf`. Set repository Actions variable
   `NEXORA_PERF_RUNNER_ENABLED=true`. Run **Fixed-host performance history**
   on `main` with `accept_baseline=true` and `full=true` for the initial
   reviewed baseline. Never run untrusted pull-request code on this host.
5. Configure the PyPI Trusted Publisher for project `NexoraDataBase`, owner
   `BlackProgrammer-prog`, repository `NexoraDB`, workflow filename
   `publish-pypi.yml`, and GitHub environment `pypi`. Create/configure that
   environment in GitHub, preferably with deployment approval. No runner
   registration token belongs in repository files or PyPI secrets.
6. Create tag **v0.3.0** at the reviewed `main` commit and publish its GitHub
   Release, using `v0.3.0.md` as the release notes. Publishing the release
   triggers the PyPI workflow: this is a real external publication step.
   The workflow validates version agreement and main ancestry, builds/tests
   the native module, checks the wheel, and verifies an isolated install.
7. Confirm PyPI publication and install the released wheel in a fresh Linux
   CPython 3.10 environment. Run
   `scripts/verify_installed_package.py --expected 0.3.0` from outside
   an importable source checkout. Check the workflow artifacts and retain the
   release test/benchmark evidence.

The publisher rejects manual publishing from a branch other than `main`.
Tag publication requires the tagged commit to be in `main` history. These
guards do not replace review or required sanitizer status checks.

References: [PyPI Trusted Publishers](https://docs.pypi.org/trusted-publishers/adding-a-publisher/)
and [GitHub releases](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).
