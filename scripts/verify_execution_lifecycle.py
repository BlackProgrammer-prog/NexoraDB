"""Owned subprocess SIGTERM campaign: active native transaction, cursor and admission."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "NexoraDB/src"))


def load_native(path):
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import load_native_module
    return load_native_module(AdminApiSettings(native_module_path=path))


def worker(args):
    from fastapi import FastAPI
    import uvicorn
    from nexoradb.commands.server import ControlledServer
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.admission import AdmissionMiddleware
    from nexoradb_admin.execution_control import current_execution, ExecutionStopped
    from nexoradb_admin.lifecycle import install_lifecycle
    native = load_native(args.native)
    engine = native.DocEngine(str(args.directory/"db"));engine.pause_document_migration()
    assert engine.create_collection("items").success
    assert engine.insert_many("items", ['{"_id":"committed"}', '{"_id":"committed2"}']).success
    owners = {"engine":engine}
    app = FastAPI();app.state.settings = AdminApiSettings()
    lifecycle = install_lifecycle(app, lambda:(owners["engine"],None), lambda:owners.clear())
    app.add_middleware(AdmissionMiddleware, tracker=lifecycle.requests)

    @app.get("/ready")
    def ready(): return {"ready":True}

    @app.get("/work")
    def work():
        token = current_execution.get()
        with lifecycle.controller.acquire("signal-probe", 16, cancellation=token):
            scope = engine.execution_scope(10000);token.attach(scope)
            with scope:
                stream = engine.iter_documents("items",page_size=1)
                next(stream)
                tx = engine.begin_transaction()
                assert engine.insert_one_tx(tx,"items",'{"_id":"uncommitted"}').success
                (args.directory/"work-started").write_text("staged")
                try:
                    while not token.event.wait(.005): token.check()
                    token.check()
                    raise AssertionError("cancelled request continued")
                except ExecutionStopped:
                    assert engine.rollback_transaction(tx).success
                    return {"success":False,"outcome":"rolled_back"}
                finally:
                    stream.close();token.native=None

    server = ControlledServer(uvicorn.Config(app,host="127.0.0.1",port=args.port,log_level="warning"))
    server.run()
    report = lifecycle.report()
    report["memory"] = json.loads(engine.memory_report())
    report["resources"] = json.loads(engine.execution_budget().report())
    (args.directory/"shutdown.json").write_text(json.dumps(report,indent=2))


def wait_until(predicate, timeout):
    until = time.monotonic()+timeout
    while time.monotonic()<until:
        if predicate(): return
        time.sleep(.02)
    raise AssertionError("subprocess readiness deadline exceeded")


def check_server_running(child, directory):
    code = child.poll()
    if code is None:
        return
    log = directory / 'stderr.log'
    try:
        detail = log.read_text(errors='replace')[-8000:]
    except OSError as error:
        detail = f'Unable to read stderr: {error}'
    raise AssertionError(f'server exited during startup (exit {code}); see {log}\n{detail}')


def campaign(args):
    args.directory.mkdir(parents=True,exist_ok=False)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1",0));port = listener.getsockname()[1]
    command = [sys.executable,str(Path(__file__).resolve()),"--worker","--directory",str(args.directory),
               "--native",str(args.native),"--port",str(port)]
    with (args.directory/"stdout.log").open("w") as out, (args.directory/"stderr.log").open("w") as err:
        child = subprocess.Popen(command,cwd=ROOT,stdout=out,stderr=err)
        try:
            def ready():
                check_server_running(child, args.directory)
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{port}/ready",timeout=.2) as response:
                        return response.status==200
                except OSError: return False
            wait_until(ready,15)
            with ThreadPoolExecutor(1) as requests:
                def request():
                    with urllib.request.urlopen(f"http://127.0.0.1:{port}/work",timeout=15) as response:
                        return json.load(response)
                future = requests.submit(request)
                wait_until(lambda:(args.directory/"work-started").exists(),10)
                started = time.monotonic();child.send_signal(signal.SIGTERM)
                response = future.result(timeout=10)
                assert response=={"success":False,"outcome":"rolled_back"}
                assert child.wait(timeout=10)==0
                elapsed = time.monotonic()-started
        finally:
            if child.poll() is None:
                # Only this owned disposable probe process; record escalation.
                (args.directory/"probe-escalation.txt").write_text("probe timed out; owned child killed")
                child.kill();child.wait()
    shutdown = json.loads((args.directory/"shutdown.json").read_text())
    assert shutdown["stage"]=="closed" and shutdown["closed"] and not shutdown["error"]
    assert shutdown["active_requests"]==shutdown["admission"]["running"]==0
    assert shutdown["resources"]["reserved_bytes"]==0
    assert shutdown["memory"]["page_live_snapshots"]==0
    native = load_native(args.native)
    recovered = native.DocEngine(str(args.directory/"db"));recovered.pause_document_migration()
    assert recovered.find_by_id("items","committed").success
    assert not recovered.find_by_id("items","uncommitted").success
    assert recovered.get_collection_size("items")==2
    assert recovered.prepare_shutdown().success
    result = dict(status="passed",signal="SIGTERM",elapsed_seconds=elapsed,
                  native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
                  sources_sha256={str(p):hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in (
                      "NexoraDB/src/nexoradb_admin/execution_control.py", "NexoraDB/src/nexoradb_admin/lifecycle.py",
                      "core/ExecutionControl.h", "NexoraDB/src/nexoradb/commands/server.py")},
                  committed_prefix_preserved=True,active_transaction="rolled_back",
                  snapshots_released=True,admission_released=True,shutdown=shutdown)
    (args.directory/"report.json").write_text(json.dumps(result,indent=2))
    print("SIGTERM: request drained, active transaction rolled back, cursor released, durable reopen passed")


if __name__=="__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory",type=Path,required=True)
    parser.add_argument("--native",type=Path,required=True)
    parser.add_argument("--worker",action="store_true")
    parser.add_argument("--port",type=int,default=0)
    args = parser.parse_args();args.directory=args.directory.resolve();args.native=args.native.resolve()
    worker(args) if args.worker else campaign(args)
