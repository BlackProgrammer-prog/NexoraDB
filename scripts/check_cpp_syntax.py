"""Check translation units using an existing compilation database; write no objects."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", default="build/debug/compile_commands.json")
    parser.add_argument("files", nargs="+")
    options = parser.parse_args()
    wanted = {Path(name).resolve() for name in options.files}
    entries = json.loads(Path(options.database).read_text())
    checked = set()
    for entry in entries:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        source = source.resolve()
        if source not in wanted or source in checked:
            continue
        args = entry.get("arguments") or shlex.split(entry["command"])
        filtered = []
        skip = False
        for arg in args:
            if skip:
                skip = False
                continue
            if arg in {"-o", "-MF", "-MT", "-MQ"}:
                skip = True
            elif arg not in {"-c", "-MD", "-MMD", "-MP"}:
                filtered.append(arg)
        filtered.append("-fsyntax-only")
        print(f"Checking {source}", flush=True)
        subprocess.run(filtered, cwd=entry["directory"], check=True)
        checked.add(source)
    missing = wanted - checked
    if missing:
        parser.error("Missing compilation entries: " + ", ".join(map(str, missing)))


if __name__ == "__main__":
    main()
