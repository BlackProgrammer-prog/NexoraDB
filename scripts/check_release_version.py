#!/usr/bin/env python3
"""Validate that the independently consumed NexoraDB version files agree."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require_match(pattern: str, text: str, source: Path) -> str:
    match = re.search(pattern, text, flags=re.MULTILINE)
    if match is None:
        raise RuntimeError(f"Could not read a version from {source}")
    return match.group(1)


def versions() -> dict[str, str]:
    cmake_path = ROOT / "CMakeLists.txt"
    python_version_path = ROOT / "NexoraDB/src/nexoradb/_version.py"
    pyproject_path = ROOT / "NexoraDB/pyproject.toml"
    vcpkg_path = ROOT / "vcpkg.json"

    cmake = require_match(
        r"project\(NexoraDB\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        cmake_path.read_text(encoding="utf-8"),
        cmake_path,
    )
    python_module = require_match(
        r'^__version__\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
        python_version_path.read_text(encoding="utf-8"),
        python_version_path,
    )
    pyproject = require_match(
        r'^version\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"',
        pyproject_path.read_text(encoding="utf-8"),
        pyproject_path,
    )
    with vcpkg_path.open(encoding="utf-8") as stream:
        vcpkg = json.load(stream)["version"]

    return {
        "CMake": cmake,
        "Python module": python_module,
        "Python package": pyproject,
        "vcpkg manifest": vcpkg,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected", help="Expected release version without a v prefix")
    arguments = parser.parse_args()
    discovered = versions()
    expected = arguments.expected or next(iter(discovered.values()))
    mismatches = {
        source: version
        for source, version in discovered.items()
        if version != expected
    }
    for source, version in discovered.items():
        print(f"{source}: {version}")
    if mismatches:
        print(f"Version validation failed; expected {expected}", file=sys.stderr)
        return 1
    print(f"All release versions match {expected}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
