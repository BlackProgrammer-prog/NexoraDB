#!/usr/bin/env python3
"""Exercise query delivery and a headless TUI using only an installed package."""

import argparse
import asyncio
import json
import os
import re
import tempfile
from importlib.metadata import version
from pathlib import Path

import httpx
import nexoradb
import socketio
from aiohttp import web
from fastapi.testclient import TestClient
from nexoradb.cli.main import NexoraCliApp
from nexoradb.cli.monitoring import MonitoringClient
from nexoradb.cli.query_client import QueryClient
from nexoradb.cli.query_editor import QueryEditor
from nexoradb.cli.query_workspace import QueryWorkspace
from nexoradb_admin.app import create_app
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.security import create_access_token
from textual.app import App


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", required=True)
    expected = parser.parse_args().expected
    assert version("NexoraDataBase") == nexoradb.__version__ == expected
    assert nexoradb.build_info()["project_version"] == expected
    assert hasattr(nexoradb.DocEngine, "iter_join_documents")
    with tempfile.TemporaryDirectory(
        prefix="nexora-installed-workspaces-"
    ) as directory:
        os.chdir(directory)
        root = Path(directory)
        settings = AdminApiSettings(
            auth_secret="installed-smoke-" + "x" * 48,
            environment="development",
            db_path=root / "db",
            graph_dir=root / "graphs",
            rocks_auto_tune=False,
            rocks_memory_mb=32,
            execution_memory_mb=32,
        )
        app = create_app(settings=settings)
        with TestClient(app) as client:
            assert (
                client.post(
                    "/auth/register",
                    json={
                        "firstName": "Release",
                        "lastName": "Smoke",
                        "email": "release@example.test",
                        "password": "ReleaseSmoke123!",
                    },
                ).status_code
                == 201
            )
            token = create_access_token(
                username="root", role="admin", settings=settings
            )
            client.headers["Authorization"] = "Bearer " + token
            dashboard = client.get("/admin/")
            assert (
                dashboard.status_code == 200
                and dashboard.headers["cache-control"] == "no-store"
            )
            assert "script-src 'self'" in dashboard.headers["content-security-policy"]
            assert client.get("/admin/brand/nexoradb-logo.svg").status_code == 200
            for asset in re.findall(
                r'(?:src|href)="(/admin/assets/[^"]+)"', dashboard.text
            ):
                response = client.get(asset)
                assert (
                    response.status_code == 200
                    and "immutable" in response.headers["cache-control"]
                )
            query_chunk = next(
                (Path(nexoradb.__file__).parent / "dashboard/static/assets").glob(
                    "QueryPage-*.js"
                )
            )
            assert client.get("/admin/assets/" + query_chunk.name).status_code == 200
            engine = app.state.engine
            assert engine.create_collection("items").success
            assert engine.create_collection("targets").success
            assert engine.insert_one(
                "items", '{"_id":"one","target":"target","value":1}'
            ).success
            assert engine.insert_one(
                "targets", '{"_id":"target","label":"joined"}'
            ).success
            query = "SELECT * FROM items LOOKUP JOIN targets ON target = _id;"
            response = client.post("/query/stream", json={"query": query})
            assert response.status_code == 200, response.text
            frames = [json.loads(line) for line in response.text.splitlines()]
            assert (
                frames[-1] == {"end": True}
                and frames[0]["document"]["__joined__"]["label"] == "joined"
            )
            cursor = client.post(
                "/query/cursors", json={"query": query, "pageSize": 1}
            ).json()
            assert cursor["rows"][0]["__joined__"]["label"] == "joined"
            if cursor["cursorId"]:
                assert (
                    client.delete("/query/cursors/" + cursor["cursorId"]).status_code
                    == 204
                )
            write = "INSERT INTO items VALUES ($record); SELECT * FROM items WHERE _id = 'once';"
            payload = {
                "query": write,
                "parameters": {"record": {"_id": "once", "value": 2}},
            }
            header = {"Idempotency-Key": "installed-release-smoke"}
            first = client.post("/query/cursors", json=payload, headers=header)
            second = client.post("/query/cursors", json=payload, headers=header)
            assert first.status_code == second.status_code == 200, (
                first.text,
                second.text,
            )
            assert first.json()["rows"] == second.json()["rows"]
            assert engine.get_collection_size("items") == 2
            assert (
                client.get("/query/requests/installed-release-smoke").json()["status"]
                == "completed"
            )
            for result in (first.json(), second.json()):
                if result["cursorId"]:
                    client.delete("/query/cursors/" + result["cursorId"])

            async def terminal():
                transport = QueryClient(
                    "http://installed", token, transport=httpx.ASGITransport(app=app)
                )
                workspace = QueryWorkspace("http://installed", token, client=transport)

                class Console(App):
                    def compose(self):
                        yield workspace

                async with Console().run_test(size=(100, 45)):
                    workspace.query_one(QueryEditor).load_text(query)
                    workspace.action_run_query()
                    await workspace.operation
                    rows = workspace.preview.snapshot()
                    assert (
                        len(rows) == 1 and rows[0]["__joined__"]["label"] == "joined"
                    ), rows
                assert transport.client.is_closed
                assert issubclass(NexoraCliApp, App)

                service = socketio.AsyncServer(async_mode="aiohttp")
                monitoring_app = web.Application()
                service.attach(monitoring_app)

                @service.event
                async def connect(sid, environ, auth):
                    assert auth == {"token": token}
                    await service.emit(
                        "metrics",
                        {"databaseHealthy": True, "requestsPerSecond": 7},
                        to=sid,
                    )

                runner = web.AppRunner(monitoring_app)
                await runner.setup()
                site = web.TCPSite(runner, "127.0.0.1", 0)
                await site.start()
                port = site._server.sockets[0].getsockname()[1]
                received = asyncio.Event()
                samples = []

                def consume(metrics):
                    samples.append(metrics)
                    received.set()

                monitor = MonitoringClient(
                    f"http://127.0.0.1:{port}", token, consume, lambda status: None
                )
                task = asyncio.create_task(monitor.connect())
                try:
                    await asyncio.wait_for(received.wait(), 5)
                    assert (
                        samples[0].source == "live"
                        and samples[0].requests_per_second == 7
                    )
                finally:
                    await monitor.disconnect()
                    await asyncio.wait_for(task, 5)
                    await service.shutdown()
                    await runner.cleanup()

            asyncio.run(terminal())
            assert not app.state.query_cursors.entries
        assert app.state.lifecycle.closed and app.state.engine is None
    print(
        json.dumps(
            {
                "status": "passed",
                "version": expected,
                "package": nexoradb.__file__,
                "checks": [
                    "bundled dashboard, logo, query chunk and CSP",
                    "installed grammar/CLI modules",
                    "native JOIN NDJSON",
                    "managed cursor",
                    "parameterized write replay",
                    "durable receipts",
                    "headless TUI",
                    "installed async monitoring transport",
                    "ordered shutdown",
                ],
            }
        )
    )


if __name__ == "__main__":
    main()
