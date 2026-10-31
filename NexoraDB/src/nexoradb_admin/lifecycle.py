"""Ordered service shutdown. Drain expiry retains resources; never kills a worker."""
from collections import deque
from contextlib import asynccontextmanager
import logging
import time
import anyio

from .execution_control import ExecutionController, RequestTracker, AdmissionPolicy


class ServiceLifecycle:
    def __init__(self, resources, release, *, drain_seconds=30, controller=None):
        if not 0 < drain_seconds <= 300: raise ValueError("drain must be 0..300 seconds")
        self.controller = controller or ExecutionController()
        self.requests = RequestTracker()
        self.resources, self.release = resources, release
        self.drain_seconds = drain_seconds
        self.events = deque(maxlen=32)
        self.stage = "running"
        self.error = None
        self.closed = False
        self.started = None
        self.background = None

    def mark(self, stage, **details):
        self.stage = stage
        event = dict(stage=stage, at_monotonic=time.monotonic(), **details)
        self.events.append(event)
        logging.getLogger(__name__).info("service_shutdown %s", event)

    def report(self):
        return dict(stage=self.stage, error=self.error, closed=self.closed,
                    active_requests=self.requests.active(), admission=self.controller.report(),
                    events=list(self.events))

    def stop_admission(self, reason="shutdown"):
        if not self.requests.stopping:
            self.mark("stop_admission", reason=reason)
        if self.background is not None: self.background.stop_admission()
        self.requests.stop()
        self.controller.stop()

    async def shutdown(self):
        if self.closed: return self.report()
        self.error = None
        self.started = time.monotonic()
        self.stop_admission()
        try:
            self.mark("drain", cancellation_requested=True)
            until = time.monotonic()+self.drain_seconds
            while self.requests.active() or self.controller.report()["running"] or self.controller.report()["queued"]:
                if time.monotonic() >= until:
                    raise RuntimeError("shutdown drain deadline exceeded; resources retained; no worker was killed")
                await anyio.sleep(.01)
            engine, manager = self.resources()  # No lazy initialization during shutdown.
            self.mark("stop_workers")
            if self.background is not None: await self.background.shutdown()
            if manager is not None:
                self.mark("release_graph_snapshots_flush")
                if not await anyio.to_thread.run_sync(manager.prepare_shutdown):
                    raise RuntimeError("graph shutdown durability barrier failed")
            if engine is not None:
                self.mark("release_document_snapshots_flush")
                prepare = getattr(engine, "prepare_shutdown", None)
                if prepare is not None:
                    result = await anyio.to_thread.run_sync(prepare)
                    if not result.success:
                        raise RuntimeError("document shutdown failed: " + result.error_msg)
            self.mark("close")
            if manager is not None:
                await anyio.to_thread.run_sync(manager.shutdown)
            self.release()  # Drop service-owned references after all workers/requests drain.
            self.closed = True
            self.mark("closed", elapsed_seconds=time.monotonic()-self.started)
        except BaseException as exc:
            self.error = str(exc)
            self.mark("failed", error=self.error, resources_retained=True)
            raise
        return self.report()


def install_lifecycle(app, resources, release):
    settings = app.state.settings
    policy = AdmissionPolicy(query_slots=settings.execution_query_slots, owner_slots=settings.execution_per_app,
                             queue_capacity=settings.execution_queue_size,
                             max_wait_seconds=settings.execution_wait_ms/1000)
    lifecycle = ServiceLifecycle(resources, release, drain_seconds=settings.shutdown_drain_ms/1000,
                                 controller=ExecutionController(policy))
    lifecycle.requests.timeout_seconds = settings.execution_timeout_ms/1000
    app.state.lifecycle = lifecycle
    app.state.execution_controller = lifecycle.controller
    previous = app.router.lifespan_context

    @asynccontextmanager
    async def lifespan(application):
        async with previous(application):
            try:
                yield
            finally:
                with anyio.CancelScope(shield=True):
                    await lifecycle.shutdown()
    app.router.lifespan_context = lifespan
    return lifecycle
