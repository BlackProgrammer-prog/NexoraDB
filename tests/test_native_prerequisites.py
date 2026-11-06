"""A working Autoconf executable alone must not pass the cold-build preflight."""

import os
import shlex
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("archive_present", [False, True])
def test_preflight_checks_archive_macros_not_only_programs(tmp_path, archive_present):
    programs = tmp_path / "bin"
    programs.mkdir()
    macros = tmp_path / "macros"
    macros.mkdir()
    # Stand-in executables make this test independent of runner packages.
    for name in (
        "cmake",
        "make",
        "gcc-11",
        "g++-11",
        "ninja",
        "pkg-config",
        "autoconf",
        "autoreconf",
        "automake",
        "aclocal",
        "libtoolize",
    ):
        executable = programs / name
        body = "exit 0"
        if name == "aclocal":
            body = "printf '%s\\n' " + shlex.quote(str(macros))
        executable.write_text("#!/bin/sh\n" + body + "\n")
        executable.chmod(0o755)
    if archive_present:
        for name in ("ax_check_compile_flag", "ax_check_link_flag"):
            (macros / (name + ".m4")).write_text("# test macro\n")
    result = subprocess.run(
        ["/bin/bash", str(ROOT / "scripts/native_prerequisites.sh"), "--check"],
        env={**os.environ, "PATH": str(programs)},
        capture_output=True,
        text=True,
    )
    assert result.returncode == (0 if archive_present else 1)
    if not archive_present:
        assert "Missing autoconf-archive macro" in result.stderr
        assert "--install" in result.stderr


def test_ci_installs_prerequisites_and_exercises_uncached_dependencies():
    for name in ("native-quality.yml", "publish-pypi.yml"):
        workflow = (ROOT / ".github/workflows" / name).read_text()
        assert "bash scripts/native_prerequisites.sh --install" in workflow
    native = (ROOT / ".github/workflows/native-quality.yml").read_text()
    assert "export VCPKG_BINARY_SOURCES=clear" in native
    assert "vcpkg/buildtrees/*/*.log" in native
