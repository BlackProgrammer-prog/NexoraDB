import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "NexoraDB/src"))
from nexoradb_admin.execution_control import (AdmissionPolicy, Cancellation, ExecutionController,
    ExecutionStopped, Overloaded, current_execution)
from nexoradb_admin.lifecycle import ServiceLifecycle
from nexoradb_admin.admission import AdmissionMiddleware
from test_resource_budget import native


def wait_for(predicate, timeout=2):
    until = time.monotonic()+timeout
    while not predicate():
        if time.monotonic() >= until: raise AssertionError("condition did not become true")
        time.sleep(.001)


def test_query_owner_cost_queue_caps_and_independent_maintenance():
    policy = AdmissionPolicy(query_slots=2, owner_slots=1, owner_queue=1, queue_capacity=2, small_slots=0,
                             max_wait_seconds=1)
    controller = ExecutionController(policy)
    with controller.acquire("a", 40), controller.acquire("b", 40):
        with controller.acquire("checkpoint", 20, lane="maintenance", wait_seconds=0):
            assert controller.report()["running_cost"] == 100
            assert controller.report()["maintenance_running"] == 1
        with pytest.raises(Overloaded): controller.acquire("a", 41)
        with pytest.raises(Overloaded): controller.acquire("c", 1, wait_seconds=0)
        assert controller.report()["running"] == 2
    assert controller.report()["running"] == 0
    assert controller.report()["maintenance_admitted_total"] == 1
    assert controller.report()["rejected_total"] == 2


def test_default_policy_protects_small_query_and_maintenance_from_full_heavy_lane():
    controller = ExecutionController()
    with controller.acquire("a", 32), controller.acquire("b", 31), controller.acquire("c", 9):
        with pytest.raises(Overloaded): controller.acquire("more-heavy", 16, wait_seconds=0)
        # Includes small work from an app already running a large query.
        with controller.acquire("a", 8, wait_seconds=0):
            with controller.acquire("checkpoint", 20, lane="maintenance", wait_seconds=0):
                assert controller.report()["running_cost"] == 100
        assert controller.report()["running_cost"] == 72
    assert controller.report()["running"] == 0


def test_full_heavy_queue_preserves_small_and_maintenance_admission_entries():
    controller = ExecutionController(AdmissionPolicy(query_slots=2, queue_capacity=4, max_wait_seconds=2))
    with ThreadPoolExecutor(3) as workers:
        active = controller.acquire("active", 16)
        first = workers.submit(controller.acquire, "first", 16)
        wait_for(lambda: controller.report()["queued"] == 1)
        second = workers.submit(controller.acquire, "second", 16)
        wait_for(lambda: controller.report()["queued"] == 2)
        with pytest.raises(Overloaded, match="queue full"): controller.acquire("third-heavy", 16)
        with controller.acquire("small", 1, wait_seconds=0):
            with controller.acquire("maintenance", 20, lane="maintenance", wait_seconds=0):
                assert controller.report()["maintenance_running"] == 1
        active.close()
        first.result(timeout=2).close();second.result(timeout=2).close()


def test_bounded_queue_owner_waiters_and_release():
    controller = ExecutionController(AdmissionPolicy(query_slots=1, owner_slots=1, small_slots=0,
        queue_capacity=2, owner_queue=1, max_wait_seconds=1))
    with ThreadPoolExecutor(2) as workers:
        active = controller.acquire("active")
        first = workers.submit(controller.acquire, "queued")
        wait_for(lambda: controller.report()["queued"] == 1)
        with pytest.raises(Overloaded, match="queue full"): controller.acquire("queued")
        second = workers.submit(controller.acquire, "other")
        wait_for(lambda: controller.report()["queued"] == 2)
        with pytest.raises(Overloaded, match="queue full"): controller.acquire("third")
        active.close(); permit = first.result(timeout=2); permit.close()
        second.result(timeout=2).close()
    report = controller.report()
    assert report["queued"] == report["running"] == 0
    assert report["waited_total"] == 2 and report["wait_seconds_sum"] > 0


def test_small_work_bypasses_heavy_only_bounded_times_then_drains():
    controller = ExecutionController(AdmissionPolicy(query_slots=4, owner_slots=4, owner_cost=80, small_slots=0,
        max_bypass=2, age_seconds=5, max_wait_seconds=2))
    active = controller.acquire("load", 70)
    with ThreadPoolExecutor(2) as workers:
        heavy = workers.submit(controller.acquire, "heavy", 80)
        wait_for(lambda: controller.report()["queued"] == 1)
        for _ in range(2): controller.acquire("small", 1, wait_seconds=0).close()
        with pytest.raises(Overloaded): controller.acquire("small", 1, wait_seconds=0)
        small = workers.submit(controller.acquire, "small", 1)
        wait_for(lambda: controller.report()["queued"] == 2)
        active.close()
        heavy.result(timeout=2).close()
        small.result(timeout=2).close()
    assert controller.report()["running_cost"] == 0


def test_shutdown_wakes_queued_and_cancels_active_without_freeing_it_early():
    controller = ExecutionController(AdmissionPolicy(query_slots=1, max_wait_seconds=2, small_slots=0))
    active = controller.acquire("active")
    with ThreadPoolExecutor(1) as workers:
        waiting = workers.submit(controller.acquire, "waiting")
        wait_for(lambda: controller.report()["queued"] == 1)
        controller.stop()
        with pytest.raises(ExecutionStopped): waiting.result(timeout=1)
    with pytest.raises(ExecutionStopped): active.cancellation.check()
    assert controller.report()["running"] == 1
    with pytest.raises(Overloaded): controller.acquire("new")
    active.close(); active.close()
    assert controller.report()["running"] == 0


def test_deadline_wait_and_policy_validation():
    controller = ExecutionController(AdmissionPolicy(query_slots=1, small_slots=0))
    with controller.acquire("active"):
        started = time.monotonic()
        with pytest.raises(Overloaded): controller.acquire("waiting", wait_seconds=.02)
        assert .01 <= time.monotonic()-started < .5
        assert controller.report()["expired_total"] == 1
    token = Cancellation(.001); time.sleep(.003)
    with pytest.raises(ExecutionStopped): controller.acquire("expired", cancellation=token)
    assert controller.report()["queued"] == 0
    for params in ({"owner_slots":0}, {"maintenance_cost":100}, {"max_wait_seconds":6}):
        with pytest.raises(ValueError): AdmissionPolicy(**params)


@pytest.mark.parametrize("failure", [None, "graph", "document"])
def test_shutdown_order_drain_before_flush_and_failure_retains_resources(failure):
    async def run():
        order = []
        class Graph:
            def prepare_shutdown(self): order.append("graph_prepare"); return failure != "graph"
            def shutdown(self): order.append("graph_close")
        class Engine:
            def prepare_shutdown(self):
                order.append("doc_prepare")
                return SimpleNamespace(success=failure != "document", error_msg="injected IO failure")
        lifecycle = ServiceLifecycle(lambda: (Engine(), Graph()), lambda: order.append("release"), drain_seconds=.5)
        request = lifecycle.requests.enter()
        active = lifecycle.controller.acquire("work", cancellation=request)
        task = asyncio.create_task(lifecycle.shutdown())
        await asyncio.sleep(.02)
        assert order == [] and request.event.is_set()
        assert lifecycle.report()["stage"] == "drain"
        active.close(); lifecycle.requests.leave(request)
        if failure:
            with pytest.raises(RuntimeError): await task
            assert lifecycle.stage == "failed" and not lifecycle.closed
            assert "release" not in order and "graph_close" not in order
        else:
            await task
            assert order == ["graph_prepare", "doc_prepare", "graph_close", "release"]
            await lifecycle.shutdown()  # Idempotent.
            assert order.count("release") == 1 and lifecycle.closed
    asyncio.run(run())


def test_drain_timeout_keeps_owners_and_records_intervention():
    async def run():
        released = []
        lifecycle = ServiceLifecycle(lambda: pytest.fail("resources touched before drain"),
                                     lambda: released.append(True), drain_seconds=.02)
        token = lifecycle.requests.enter()
        with pytest.raises(RuntimeError, match="resources retained"): await lifecycle.shutdown()
        assert not released and lifecycle.requests.active() == 1
        assert lifecycle.report()["events"][-1]["resources_retained"]
        lifecycle.requests.leave(token)
    asyncio.run(run())


def test_http_stop_admission_and_full_response_lifetime():
    async def run():
        entered, release = asyncio.Event(), asyncio.Event()
        async def application(scope, receive, send):
            assert current_execution.get() is not None
            entered.set(); await release.wait()
        middleware = AdmissionMiddleware(application, 2)
        async def receive(): return {"type":"http.request", "body":b""}
        messages = []
        async def send(message): messages.append(message)
        scope = {"type":"http", "method":"GET", "path":"/"}
        first = asyncio.create_task(middleware(scope, receive, send)); await entered.wait()
        middleware.tracker.stop()
        await middleware(scope, receive, send)
        assert messages[0]["status"] == 503
        assert middleware.tracker.active() == 1
        release.set(); await first
        assert middleware.active == middleware.tracker.active() == 0
        assert current_execution.get() is None
    asyncio.run(run())


def test_request_cancellation_waits_for_real_worker_before_releasing_slot():
    async def run():
        import anyio
        entered, release = threading.Event(), threading.Event()
        def work(): entered.set(); release.wait(timeout=2)
        async def application(scope, receive, send): await anyio.to_thread.run_sync(work)
        middleware = AdmissionMiddleware(application, 1)
        async def receive(): return {"type":"http.request", "body":b""}
        task = asyncio.create_task(middleware({"type":"http"}, receive, None))
        while not entered.is_set(): await asyncio.sleep(.001)
        task.cancel(); await asyncio.sleep(.02)
        assert not task.done() and middleware.tracker.active() == 1
        release.set()
        with pytest.raises(asyncio.CancelledError): await task
        assert middleware.active == middleware.tracker.active() == 0
    asyncio.run(run())


def test_monitoring_uses_reserved_maintenance_share_under_query_load():
    async def run():
        from nexoradb_admin.monitoring import MonitoringSocketServer, MonitoringState
        from nexoradb_admin.config import AdminApiSettings
        class Engine:
            def is_healthy(self): time.sleep(.02); return True
            def get_ram_usage_bytes(self): return 1
            def get_disk_usage_bytes(self): return 2
            def list_collections(self): return []
        monitor = MonitoringSocketServer(settings=AdminApiSettings(), state=MonitoringState(), engine_provider=Engine)
        monitor.lifecycle = ServiceLifecycle(lambda: (None, None), lambda: None)
        with monitor.lifecycle.controller.acquire("a", 32), monitor.lifecycle.controller.acquire("b", 32):
            task = asyncio.create_task(monitor.collect_metrics())
            await asyncio.sleep(.005)
            assert monitor.lifecycle.controller.report()["maintenance_running"] == 1
            assert (await task)["databaseHealthy"]
        monitor.stop_admission()
        with pytest.raises(RuntimeError, match="stopping"): await monitor.collect_metrics()
        await monitor.shutdown()
        assert monitor.lifecycle.requests.active() == 0
    asyncio.run(run())


def test_native_cancel_before_commit_rolls_back_and_retains_previous_commit(tmp_path, native):
    engine = native.DocEngine(str(tmp_path/"db"));engine.pause_document_migration()
    assert engine.create_collection("items").success
    assert engine.insert_one("items", '{"_id":"committed"}').success
    scope = engine.execution_scope(10000)
    with scope:
        tx = engine.begin_transaction()
        assert engine.insert_one_tx(tx, "items", '{"_id":"staged"}').success
        scope.cancel()
        with pytest.raises(InterruptedError): engine.commit_transaction(tx)
        assert engine.rollback_transaction(tx).success  # Rollback stays available after cancellation.
    assert engine.find_by_id("items", "committed").success
    assert not engine.find_by_id("items", "staged").success
    assert json.loads(engine.execution_budget().report())["components"]["buffer"] == 0


def test_point_select_and_metadata_count_use_small_share_when_heavy_is_full(tmp_path, native):
    from nexoradb_admin.query_runner import execute_query, QueryExecuteRequest
    from fastapi import HTTPException
    engine = native.DocEngine(str(tmp_path/"db"));engine.pause_document_migration()
    assert engine.create_collection("items").success
    assert engine.insert_one("items", '{"_id":"a"}').success
    controller = ExecutionController(AdmissionPolicy(max_wait_seconds=.01))
    with controller.acquire("heavy-a",32), controller.acquire("heavy-b",31), controller.acquire("heavy-c",9):
        result = execute_query(engine=engine,graph_manager=None,controller=controller,owner="reader",
                               payload=QueryExecuteRequest(query="SELECT * FROM items WHERE _id = 'a';"))
        assert result.rows[0]["_id"] == "a"
        execute_query(engine=engine,graph_manager=None,controller=controller,owner="reader",
                      payload=QueryExecuteRequest(query="COUNT FROM items;"))
        with pytest.raises(HTTPException) as error:
            execute_query(engine=engine,graph_manager=None,controller=controller,owner="reader",
                          payload=QueryExecuteRequest(query="SELECT * FROM items;"))
        assert error.value.status_code == 503 and error.value.detail["outcome"] == "not_started"


def test_native_stream_cancellation_cross_thread_closes_snapshot(tmp_path, native):
    engine = native.DocEngine(str(tmp_path/"db"));engine.pause_document_migration()
    assert engine.create_collection("items").success
    assert engine.insert_many("items", ['{"_id":"a"}', '{"_id":"b"}']).success
    scope = engine.execution_scope(10000)
    with scope: stream = engine.iter_documents("items", page_size=1)
    with ThreadPoolExecutor(1) as worker:
        assert worker.submit(next, stream).result()["_id"] == "a"
        scope.cancel()
        with pytest.raises(InterruptedError): worker.submit(next, stream).result()
    stream.close()
    assert json.loads(engine.memory_report())["page_live_snapshots"] == 0


def test_native_expired_scope_rejects_execution_and_nesting_restores(tmp_path, native):
    engine = native.DocEngine(str(tmp_path/"db")); engine.pause_document_migration()
    assert engine.create_collection("items").success
    with engine.execution_scope(1000):
        inner = engine.execution_scope(1)
        with inner:
            time.sleep(.005)
            result = engine.insert_one("items", '{"_id":"no"}')
            assert not result.success and result.error_code == "execution_cancelled"
        assert engine.insert_one("items", '{"_id":"yes"}').success
    assert engine.get_collection_size("items") == 1


def test_native_long_scan_deadline_has_no_partial_success_or_pinned_cursor(tmp_path, native):
    settings = native.RocksSettings(); settings.execution_budget = native.ResourceBudget(64*1024**2)
    engine = native.DocEngine(str(tmp_path/"db"), rocks_settings=settings); engine.pause_document_migration()
    assert engine.create_collection("items").success
    for start in range(0, 20000, 1000):
        assert engine.insert_many("items", [json.dumps({"_id":str(i), "v":i}) for i in range(start,start+1000)]).success
    condition = native.Condition.leaf("v", native.Op.EQ, "-1", native.ValueType.Int64)
    for _ in range(5):
        with engine.execution_scope(1):
            with pytest.raises(InterruptedError): engine.count("items", condition)
        with engine.execution_scope(1):
            stream = engine.iter_documents("items", condition, page_size=1)
            try:
                with pytest.raises(InterruptedError): next(stream)
            finally: stream.close()
        assert json.loads(engine.memory_report())["page_live_snapshots"] == 0
        assert json.loads(engine.execution_budget().report())["reserved_bytes"] == 0
    assert engine.is_healthy()


def test_query_cancelled_between_writes_reports_completed_prefix(tmp_path, native, monkeypatch):
    from nexoradb_admin import query_runner
    engine = native.DocEngine(str(tmp_path/"db"));engine.pause_document_migration()
    assert engine.create_collection("items").success
    token = Cancellation()
    parser = query_runner._load_nexoraql_package()
    original = parser.Executor.execute
    def execute(self, statement):
        result = original(self, statement)
        token.cancel("test shutdown")
        return result
    monkeypatch.setattr(parser.Executor, "execute", execute)
    from fastapi import HTTPException
    with pytest.raises(HTTPException) as error:
        query_runner.execute_query(engine=engine, graph_manager=None, cancellation=token,
            payload=query_runner.QueryExecuteRequest(query='INSERT INTO items VALUES (\'{"_id":"first"}\'); INSERT INTO items VALUES (\'{"_id":"second"}\');'))
    assert error.value.status_code == 408
    assert error.value.detail["completedStatements"][0]["success"]
    assert engine.find_by_id("items", "first").success
    assert not engine.find_by_id("items", "second").success
