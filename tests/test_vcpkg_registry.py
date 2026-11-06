"""Regression checks for registry drift between local builds and GitHub jobs."""

import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "vcpkg_registry", ROOT / "scripts/check_vcpkg_registry.py"
)
registry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(registry)


def test_every_native_workflow_uses_shared_bootstrap():
    for name in ("native-quality.yml", "publish-pypi.yml", "performance.yml"):
        text = (ROOT / ".github/workflows" / name).read_text()
        assert "bash scripts/bootstrap_vcpkg.sh" in text
        assert "git -C vcpkg checkout" not in text


def test_tsan_disables_aslr_for_dependency_probes_and_discovery():
    text = (ROOT / ".github/workflows/native-quality.yml").read_text()
    assert 'setarch "$(uname -m)" -R cmake --preset tsan-full' in text
    assert 'setarch "$(uname -m)" -R cmake --build build/tsan-full' in text
    assert 'setarch "$(uname -m)" -R ctest --preset tsan-full' in text


def test_missing_override_fails_before_cmake(tmp_path, monkeypatch):
    revision = "a" * 40
    (tmp_path / "cmake").mkdir()
    (tmp_path / "cmake/vcpkg-revision.txt").write_text(revision + "\n")
    (tmp_path / "vcpkg.json").write_text(
        json.dumps(
            {
                "builtin-baseline": "b" * 40,
                "overrides": [{"name": "libsodium", "version": "1.0.22"}],
            }
        )
    )

    def output(command, **_):
        if "rev-parse" in command:
            return revision
        if "show" in command:
            return json.dumps(
                {"versions": [{"version": "1.0.18", "git-tree": "c" * 40}]}
            )
        return ""

    monkeypatch.setattr(registry.subprocess, "check_output", output)
    with pytest.raises(ValueError, match="libsodium 1.0.22 is absent"):
        registry.check(tmp_path)
