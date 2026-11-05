"""Launch the bundled offline native IAM utility without handling credentials."""

import os
import sys
from pathlib import Path


def main() -> None:
    executable = Path(__file__).resolve().parents[1] / "native" / "nexora_iam"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise SystemExit(
            "Bundled nexora_iam executable is missing; install a complete matching wheel"
        )
    # No shell, PATH lookup, password parsing or Python credential storage.
    os.execv(str(executable), [str(executable), *sys.argv[1:]])
