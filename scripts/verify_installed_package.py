#!/usr/bin/env python3
"""Smoke-test metadata and the native module from an installed wheel."""

from __future__ import annotations

import argparse
from importlib.metadata import version

import nexoradb


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected", required=True)
    arguments = parser.parse_args()
    package_version = version("NexoraDataBase")
    build_info = nexoradb.build_info()

    assert package_version == arguments.expected
    assert nexoradb.__version__ == arguments.expected
    assert build_info["project_version"] == arguments.expected
    assert build_info["build_type"] == "Release"
    print(
        "Installed wheel verified:",
        package_version,
        build_info["build_type"],
        build_info["rocksdb_version"],
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
