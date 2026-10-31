"""Bounded admission and cooperative execution; credits are policy costs, not RSS."""
from contextvars import ContextVar
from dataclasses import dataclass
from threading import Condition, Event
import time


class ExecutionStopped(RuntimeError):
    def __init__(self, reason, *, outcome="not_started"):
        super().__init__(reason)
        self.reason, self.outcome = reason, outcome


class Overloaded(RuntimeError):
    pass


class Cancellation:
    def __init__(self, timeout_seconds=30):
        self.deadline = time.monotonic() + timeout_seconds
        self.event = Event()
        self.reason = "cancelled"
        self.native = None
        self.completed = []

    def cancel(self, reason="cancelled"):
        self.reason = reason
        self.event.set()
        native = self.native
        if native is not None:
            native.cancel()

    def check(self):
        if self.event.is_set():
            raise ExecutionStopped(self.reason)
        if time.monotonic() >= self.deadline:
            raise ExecutionStopped("deadline_exceeded")

    def attach(self, native):
        self.native = native
        if self.event.is_set():
            native.cancel()


current_execution = ContextVar("nexora_execution", default=None)


@dataclass(frozen=True)
class AdmissionPolicy:
    query_slots: int = 8
    maintenance_slots: int = 1
    cost_capacity: int = 100
    maintenance_cost: int = 20
    owner_slots: int = 2
    owner_cost: int = 40
    queue_capacity: int = 32
    owner_queue: int = 8
    max_wait_seconds: float = .25
    max_bypass: int = 8
    age_seconds: float = .1
    small_slots: int = 1
    small_cost: int = 8

    def __post_init__(self):
        for name in ("query_slots", "maintenance_slots", "cost_capacity", "maintenance_cost",
                     "owner_slots", "owner_cost", "queue_capacity", "owner_queue", "max_bypass"):
            if type(getattr(self, name)) is not int or getattr(self, name) <= 0:
                raise ValueError(name + " must be a positive integer")
        if not 0 < self.maintenance_cost < self.cost_capacity:
            raise ValueError("maintenance must have an exclusive positive cost share")
        if self.owner_cost > self.cost_capacity - self.maintenance_cost:
            raise ValueError("owner cost exceeds query share")
        if type(self.small_slots) is not int or not 0 <= self.small_slots < self.query_slots:
            raise ValueError("small slots must leave at least one large-query slot")
        if type(self.small_cost) is not int or not 0 < self.small_cost < self.owner_cost:
            raise ValueError("small cost must leave a positive large-query owner share")
        if self.small_slots and self.owner_slots < 2:
            raise ValueError("protected small work needs at least two owner slots")
        if self.small_slots and self.queue_capacity < 3:
            raise ValueError("protected lanes need at least three queue entries")
        if not 0 < self.max_wait_seconds <= 5 or not 0 < self.age_seconds <= 5:
            raise ValueError("wait/age must be in (0, 5] seconds")


class Permit:
    def __init__(self, controller, owner, lane, cost, cancellation):
        self.controller, self.owner, self.lane, self.cost = controller, owner, lane, cost
        self.cancellation = cancellation
        self.closed = False

    def close(self):
        with self.controller.changed:
            if self.closed:
                return
            self.closed = True
            self.controller.running.remove(self)
            self.controller.changed.notify_all()

    def __enter__(self): return self
    def __exit__(self, *_): self.close()


class ExecutionController:
    def __init__(self, policy=None):
        self.policy = policy or AdmissionPolicy()
        self.changed = Condition()
        self.queue = []
        self.running = []
        self.stopping = False
        self.metrics = dict(admitted_total=0, rejected_total=0, expired_total=0,
                            cancelled_total=0, waited_total=0, wait_seconds_sum=0.,
                            wait_seconds_max=0., maintenance_admitted_total=0)

    def _fits(self, item):
        p = self.policy
        lane = [r for r in self.running if r.lane == item["lane"]]
        cap = p.maintenance_cost if item["lane"] == "maintenance" else p.cost_capacity-p.maintenance_cost
        slots = p.maintenance_slots if item["lane"] == "maintenance" else p.query_slots
        if item["lane"] == "query" and p.small_slots:
            lane = [r for r in lane if (r.cost <= p.small_cost) == (item["cost"] <= p.small_cost)]
            if item["cost"] <= p.small_cost:
                cap, slots = p.small_cost, p.small_slots
            else:
                cap, slots = cap-p.small_cost, slots-p.small_slots
        if len(lane) >= slots or sum(r.cost for r in lane) + item["cost"] > cap:
            return False
        if item["lane"] == "query":
            owner = [r for r in self.running if r.lane == "query" and r.owner == item["owner"]]
            if len(owner) >= p.owner_slots or sum(r.cost for r in owner)+item["cost"] > p.owner_cost: return False
            if p.small_slots and item["cost"] > p.small_cost:
                heavy = [r for r in owner if r.cost > p.small_cost]
                return len(heavy) < p.owner_slots-1 and sum(r.cost for r in heavy)+item["cost"] <= p.owner_cost-p.small_cost
            return True
        return True

    def _next(self, lane, cost):
        waiting = [q for q in self.queue if q["lane"] == lane and
                   (lane != "query" or not self.policy.small_slots or
                    (q["cost"] <= self.policy.small_cost) == (cost <= self.policy.small_cost))]
        # Small work may bypass a temporarily blocked large request, at most N
        # times or age_seconds. Then drain this lane for its oldest waiter.
        for item in waiting:
            if self._fits(item):
                return item
            if item["bypassed"] >= self.policy.max_bypass or time.monotonic()-item["at"] >= self.policy.age_seconds:
                return None
        return None

    def acquire(self, owner, cost=1, *, lane="query", cancellation=None, wait_seconds=None):
        p = self.policy
        if not isinstance(owner, str) or not owner or len(owner) > 256 or lane not in {"query", "maintenance"}:
            raise ValueError("invalid execution owner/lane")
        cap = p.maintenance_cost if lane == "maintenance" else p.owner_cost
        if lane == "query" and p.small_slots and type(cost) is int and cost > p.small_cost: cap -= p.small_cost
        if type(cost) is not int or not 1 <= cost <= cap:
            with self.changed: self.metrics["rejected_total"] += 1
            raise Overloaded("execution cost exceeds owner/lane capacity")
        token = cancellation or Cancellation()
        wait = p.max_wait_seconds if wait_seconds is None else wait_seconds
        if not 0 <= wait <= 5: raise ValueError("admission wait must be 0..5 seconds")
        at = time.monotonic()
        item = dict(owner=owner, lane=lane, cost=cost, at=at, bypassed=0, ticket=object())
        with self.changed:
            if self.stopping:
                self.metrics["rejected_total"] += 1
                raise Overloaded("execution admission stopped")
            def category(q):
                return "maintenance" if q["lane"] == "maintenance" else "small" if q["cost"] <= p.small_cost else "large"
            category_full = False
            if p.small_slots:
                category_limit = p.queue_capacity-2 if category(item) == "large" else 1
                category_full = sum(category(q) == category(item) for q in self.queue) >= category_limit
            if category_full or len(self.queue) >= p.queue_capacity or sum(q["owner"] == owner for q in self.queue) >= p.owner_queue:
                self.metrics["rejected_total"] += 1
                raise Overloaded("execution queue full")
            self.queue.append(item)
            waited = False
            try:
                while True:
                    token.check()
                    if self.stopping: raise ExecutionStopped("shutdown")
                    if self._next(lane, cost) is item:
                        for q in self.queue:
                            if q is item: break
                            if q["lane"] == lane and (not p.small_slots or lane != "query" or
                                (q["cost"] <= p.small_cost) == (cost <= p.small_cost)): q["bypassed"] += 1
                        permit = Permit(self, owner, lane, cost, token)
                        self.running.append(permit)
                        self.metrics["admitted_total"] += 1
                        self.metrics["maintenance_admitted_total"] += lane == "maintenance"
                        return permit
                    if not waited:
                        waited = True; self.metrics["waited_total"] += 1
                    remaining = min(at+wait, token.deadline)-time.monotonic()
                    if remaining <= 0:
                        self.metrics["expired_total"] += 1
                        self.metrics["rejected_total"] += 1
                        raise Overloaded("execution admission wait expired")
                    self.changed.wait(min(.01, remaining))
            except ExecutionStopped:
                self.metrics["cancelled_total"] += 1
                raise
            finally:
                self.queue.remove(item)
                elapsed = time.monotonic()-at
                self.metrics["wait_seconds_sum"] += elapsed
                self.metrics["wait_seconds_max"] = max(self.metrics["wait_seconds_max"], elapsed)
                self.changed.notify_all()

    def stop(self):
        with self.changed:
            self.stopping = True
            for permit in self.running:
                permit.cancellation.cancel("shutdown")
            self.changed.notify_all()

    def report(self):
        with self.changed:
            return {**self.metrics, "stopping": self.stopping, "queued": len(self.queue),
                    "running": len(self.running), "running_cost": sum(r.cost for r in self.running),
                    "maintenance_running": sum(r.lane == "maintenance" for r in self.running),
                    "oldest_wait_seconds": max((time.monotonic()-q["at"] for q in self.queue), default=0),
                    "policy": dict(vars(self.policy)), "scope": "per service; costs are policy units, not bytes or RSS"}


class RequestTracker:
    """Tracks full response lifetime, including streaming and worker completion."""
    def __init__(self, timeout_seconds=30):
        self.changed = Condition()
        self.tokens = set()
        self.stopping = False
        self.timeout_seconds = timeout_seconds

    def enter(self):
        with self.changed:
            if self.stopping: raise Overloaded("service stopping")
            token = Cancellation(self.timeout_seconds)
            self.tokens.add(token)
            return token

    def leave(self, token):
        with self.changed:
            self.tokens.discard(token)
            self.changed.notify_all()

    def stop(self):
        with self.changed:
            self.stopping = True
            for token in self.tokens: token.cancel("shutdown")

    def active(self):
        with self.changed: return len(self.tokens)
