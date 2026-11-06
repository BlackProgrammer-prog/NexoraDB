#!/usr/bin/env python3
"""Check manifest overrides against the exact registry used by CI."""

import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(root=ROOT):
    revision = (root / "cmake/vcpkg-revision.txt").read_text().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("vcpkg revision must be a full commit hash")
    manifest = json.loads((root / "vcpkg.json").read_text())

    def git(*arguments):
        return subprocess.check_output(
            ["git", "-C", str(root / "vcpkg"), *arguments], text=True
        ).strip()

    if git("rev-parse", "HEAD") != revision:
        raise ValueError(
            "vcpkg checkout differs from cmake/vcpkg-revision.txt; run bootstrap_vcpkg.sh"
        )
    git("cat-file", "-e", manifest["builtin-baseline"] + ":versions/baseline.json")
    for override in manifest.get("overrides", []):
        name = override["name"]
        versions = json.loads(
            git("show", f"{revision}:versions/{name[0]}-/{name}.json")
        )
        scheme = next(
            key
            for key in ("version", "version-date", "version-semver", "version-string")
            if key in override
        )
        found = next(
            (
                item
                for item in versions["versions"]
                if item.get(scheme) == override[scheme]
                and item.get("port-version", 0) == override.get("port-version", 0)
            ),
            None,
        )
        if found is None:
            raise ValueError(
                f"{name} {override[scheme]} is absent from pinned vcpkg {revision}"
            )
        git("cat-file", "-e", found["git-tree"] + "^{tree}")
        print(f"Verified override: {name} {override[scheme]}")
    print("vcpkg registry verified: " + revision)


if __name__ == "__main__":
    check()
