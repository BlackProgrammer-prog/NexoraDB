#!/usr/bin/env python3
"""Validate installed offline IAM, native authority and public recovery flows."""

import argparse
import gc
import json
import os
import pty
import select
import subprocess
import tempfile
import time
from pathlib import Path

import nexoradb
from fastapi.testclient import TestClient
from nexoradb_admin.app import create_app
from nexoradb_admin.config import AdminApiSettings

from verify_iam_cli import terminal_enroll

PASSWORD = "Terminal-secret-test-8931!"


def recover(binary, database, factor):
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [str(binary), "recover", str(database)], stdin=slave, stdout=slave, stderr=slave
    )
    os.close(slave)
    received = bytearray()
    password = "installed owner replacement password"
    try:
        for prompt, answer in (
            (b"Offline recovery factor: ", factor),
            (b"New password (at least 15 bytes): ", password),
            (b"Confirm password: ", password),
        ):
            deadline = time.monotonic() + 20
            while prompt not in received:
                if time.monotonic() >= deadline:
                    raise RuntimeError("Installed recovery prompt timeout")
                if select.select([master], [], [], 0.1)[0]:
                    try:
                        received.extend(os.read(master, 8192))
                    except OSError as error:
                        detail = received.decode(errors="replace")
                        detail = detail.replace(factor, "[redacted]").replace(
                            password, "[redacted]"
                        )
                        raise RuntimeError(
                            "Installed recovery exited before prompt: " + detail
                        ) from error
            os.write(master, (answer + "\n").encode())
        assert process.wait(timeout=20) == 0
        while select.select([master], [], [], 0)[0]:
            try:
                data = os.read(master, 8192)
            except OSError:
                break
            if not data:
                break
            received.extend(data)
        assert factor.encode() not in received and password.encode() not in received
        return password
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", required=True)
    expected = parser.parse_args().expected
    assert nexoradb.__version__ == nexoradb.build_info()["project_version"] == expected
    binary = Path(nexoradb.__file__).parent / "native/nexora_iam"
    assert (
        subprocess.check_output([str(binary), "--version"], text=True).strip()
        == "nexora_iam " + expected
    )
    wrapper = Path(os.sys.executable).parent / "nexoradb-iam"
    assert (
        subprocess.check_output([str(wrapper), "--version"], text=True).strip()
        == "nexora_iam " + expected
    )
    with tempfile.TemporaryDirectory(prefix="nexora-installed-iam-") as directory:
        root = Path(directory)
        settings = AdminApiSettings(
            db_path=root / "db",
            graph_dir=root / "graphs",
            rocks_auto_tune=False,
            rocks_memory_mb=32,
            rocks_cache_mb=8,
        )
        factors = terminal_enroll(binary, settings.db_path)
        with TestClient(create_app(settings=settings)) as client:
            login = client.post(
                "/auth/login", json={"username": "selected.owner", "password": PASSWORD}
            )
            assert login.status_code == 200, login.text
            owner = {"Authorization": "Bearer " + login.json()["accessToken"]}
            assert login.json()["user"]["protectedRoot"]
            assert (
                client.post(
                    "/collections", headers=owner, json={"name": "items"}
                ).status_code
                == 201
            )
            created = client.post(
                "/security/users",
                headers=owner,
                json={
                    "username": "reader",
                    "password": PASSWORD,
                    "grants": {"items": ["read"]},
                },
            )
            assert created.status_code == 201, created.text
            identity = created.json()["_id"]
            token = client.post(
                "/auth/login", json={"username": "reader", "password": PASSWORD}
            ).json()["accessToken"]
            reader = {"Authorization": "Bearer " + token}
            assert (
                client.get("/collections/items/documents", headers=reader).status_code
                == 200
            )
            assert client.get("/security/users", headers=reader).status_code == 403
            factor = client.post(
                "/security/users/" + identity + "/reset", headers=owner
            ).json()["resetToken"]
            assert client.get("/auth/me", headers=reader).status_code == 401
            reset = {
                "resetToken": factor,
                "newPassword": "new installed reader password",
            }
            assert client.post("/auth/reset-password", json=reset).status_code == 200
            assert client.post("/auth/reset-password", json=reset).status_code == 401
            try:
                nexoradb.DocEngine(str(settings.db_path))
            except RuntimeError:
                pass
            else:
                raise AssertionError("Enrolled store accepted legacy engine")
        del client
        gc.collect()  # TestClient/app closures can outlive the exited ASGI lifespan.
        new_password = recover(binary, settings.db_path, factors[0])
        with TestClient(create_app(settings=settings)) as client:
            assert (
                client.post(
                    "/auth/login",
                    json={"username": "selected.owner", "password": PASSWORD},
                ).status_code
                == 401
            )
            assert (
                client.post(
                    "/auth/login",
                    json={"username": "selected.owner", "password": new_password},
                ).status_code
                == 200
            )
    print(
        json.dumps(
            {
                "status": "passed",
                "version": expected,
                "checks": [
                    "bundled native IAM and launcher",
                    "offline enrollment",
                    "native login/grants/default deny",
                    "single-use user reset/revocation",
                    "legacy constructor refusal",
                    "offline owner recovery and restart",
                ],
            }
        )
    )


if __name__ == "__main__":
    main()
