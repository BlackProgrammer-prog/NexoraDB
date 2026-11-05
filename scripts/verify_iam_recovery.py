"""Native bootstrap atomicity under SIGKILL and WAL faults; isolated temp stores."""
import argparse
import signal
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()
    probe = str(args.probe.resolve())
    scenarios = [("crash", stage, "after" if stage in (4, 5) else "either")
                 for stage in range(1, 6)]
    scenarios += [("partial", stage, "either-or-locked" if stage == 4 else "either")
                  for stage in range(1, 5)]
    with tempfile.TemporaryDirectory(prefix="nexora-iam-crash-") as root:
        for action, stage, expected in scenarios:
            path = str(Path(root) / f"{action}-{stage}")
            killed = subprocess.run([probe, path, action, str(stage)], capture_output=True,
                                    text=True, timeout=15)
            if killed.returncode != -signal.SIGKILL:
                raise RuntimeError(f"{action}/{stage}: fault missed: {killed.stderr}")
            if stage == 5 and "COMMITTED" not in killed.stdout:
                raise RuntimeError("committed crash did not acknowledge enrollment")
            verified = subprocess.run([probe, path, "verify", expected], capture_output=True,
                                      text=True, timeout=15)
            if verified.returncode:
                raise RuntimeError(f"{action}/{stage}: recovery failed: {verified.stderr}")
            print(f"{action}/{stage}: {verified.stdout.strip()}")
    print(f"{len(scenarios)} bootstrap crash/WAL scenarios passed")


if __name__ == "__main__":
    main()
