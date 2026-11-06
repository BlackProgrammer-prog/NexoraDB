"""POSIX terminal secrecy and native Python entry-point regression check."""
import argparse
import importlib.util
import os
import pty
import re
import select
import subprocess
import tempfile
import time
from pathlib import Path


def terminal_enroll(binary, database, *, cancel=False):
    master, slave = pty.openpty()
    process = subprocess.Popen([str(binary), "init", str(database)], stdin=slave,
                               stdout=slave, stderr=slave)
    os.close(slave)
    transcript = bytearray()
    consumed = 0

    def until(marker):
        nonlocal consumed
        deadline = time.monotonic() + 15
        while marker not in transcript[consumed:]:
            if time.monotonic() > deadline:
                raise RuntimeError("IAM terminal prompt timeout")
            if select.select([master], [], [], 0.1)[0]:
                try:
                    chunk = os.read(master, 8192)
                except OSError as error:
                    raise RuntimeError("IAM terminal closed before prompt") from error
                if not chunk:
                    raise RuntimeError("IAM terminal closed before prompt")
                transcript.extend(chunk)
        consumed = len(transcript)

    password = b"Terminal-secret-test-8931!"
    try:
        until(b"Owner username: ")
        os.write(master, b"selected.owner\n")
        until(b"New password (at least 15 bytes): ")
        if cancel:
            os.write(master, b"\x03")
        else:
            os.write(master, password + b"\n")
            until(b"Confirm password: ")
            os.write(master, password + b"\n")
        deadline = time.monotonic() + 15
        while process.poll() is None:
            if time.monotonic() > deadline:
                raise RuntimeError("IAM terminal command timeout")
            if select.select([master], [], [], 0.1)[0]:
                try:
                    transcript.extend(os.read(master, 8192))
                except OSError:
                    break
        result = process.wait(timeout=5)
        # Drain any last output buffered before process exit.
        while select.select([master], [], [], 0)[0]:
            try:
                data = os.read(master, 8192)
            except OSError:
                break
            if not data:
                break
            transcript.extend(data)
        if result != (1 if cancel else 0):
            raise RuntimeError("IAM interactive command returned an unexpected status")
        if password in transcript:
            raise RuntimeError("Password leaked through terminal echo")
        codes = re.findall(rb"(?m)^([0-9a-f]{64})\r?$", transcript)
        if not cancel and (len(codes) != 8 or len(set(codes)) != 8):
            raise RuntimeError("Expected exactly eight distinct recovery factors")
        for file in database.iterdir():
            if file.is_file():
                content = file.read_bytes()
                if password in content or any(code in content for code in codes):
                    raise RuntimeError("A plaintext enrollment secret persisted to disk")
        return [code.decode("ascii") for code in codes]
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--module", type=Path, required=True)
    args = parser.parse_args()
    cli = args.cli.resolve()
    spec = importlib.util.spec_from_file_location("nexoradb", args.module.resolve())
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
    with tempfile.TemporaryDirectory(prefix="nexora-iam-cli-") as root:
        database = Path(root) / "enrolled"
        terminal_enroll(cli, database)
        status = subprocess.run([str(cli), "status", str(database)], capture_output=True,
                                text=True, check=True, timeout=10)
        if "INITIALIZED" not in status.stdout or "Username: selected.owner" not in status.stdout:
            raise RuntimeError("Owner-selected identity was not persisted")
        try:
            native.DocEngine(str(database))
        except RuntimeError as error:
            if "legacy access is disabled" not in str(error):
                raise RuntimeError("Native Python access failed for an unrelated reason") from error
        else:
            raise RuntimeError("Python bypassed native IAM enrollment")
        if hasattr(native, "OfflineIdentityStore"):
            raise RuntimeError("Offline bootstrap leaked into Python bindings")
        # No secrets can enter through a pipe; cancellation never enrolls.
        piped = subprocess.run([str(cli), "init", str(Path(root) / "piped")],
                               input="owner\n", capture_output=True, text=True, timeout=10)
        if piped.returncode != 1 or "Interactive terminal required" not in piped.stderr:
            raise RuntimeError("Piped credential input was not rejected")
        cancelled = Path(root) / "cancelled"
        terminal_enroll(cli, cancelled, cancel=True)
        status = subprocess.run([str(cli), "status", str(cancelled)], capture_output=True,
                                text=True, check=True, timeout=10)
        if status.stdout.strip() != "UNINITIALIZED":
            raise RuntimeError("Cancelled enrollment consumed bootstrap")
    print("Terminal secrecy, secret persistence, cancellation and Python denial checks passed")


if __name__ == "__main__":
    main()
