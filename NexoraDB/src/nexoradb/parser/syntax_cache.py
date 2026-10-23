"""Bounded syntax-only LRU. Never stores bound parameters, results or auth decisions."""
from collections import OrderedDict
from copy import deepcopy
from threading import RLock
from time import monotonic
import sys


class SyntaxCache:
    def __init__(self, max_entries=256, max_bytes=8*1024*1024, ttl_seconds=300):
        self.lock = RLock()
        self.enabled = True
        self.max_entries, self.max_bytes, self.ttl = max_entries, max_bytes, ttl_seconds
        self.items = OrderedDict()
        self.used = self.hits = self.misses = self.evictions = self.bypasses = 0

    def _cost(self, text, tree):
        # Conservative accounted Python object bytes, not a process RSS claim.
        pending, seen, size = [text, tree], set(), 0
        while pending:
            value = pending.pop()
            if id(value) in seen:
                continue
            seen.add(id(value))
            size += sys.getsizeof(value)
            if size > min(self.max_bytes//2, 512*1024) or len(seen) > 100000:
                return None
            if isinstance(value, dict):
                pending.extend(value.keys())
                pending.extend(value.values())
            elif isinstance(value, (list, tuple)):
                pending.extend(value)
            elif hasattr(value, '__dict__'):
                pending.append(vars(value))
        return size * 2

    def get_or_build(self, text, builder):
        with self.lock:
            now = monotonic()
            if not self.enabled or len(text) > 16384:
                self.bypasses += 1
                return builder()
            for key in list(self.items):
                if self.items[key][2] <= now:
                    _, cost, _ = self.items.pop(key)
                    self.used -= cost
                    self.evictions += 1
            if text in self.items:
                self.hits += 1
                self.items.move_to_end(text)
                return deepcopy(self.items[text][0])
            self.misses += 1
            tree = builder()  # Failures are never cached. Lock prevents cold-key stampedes.
            cost = self._cost(text, tree)
            if cost is not None and self.max_entries > 0:
                while self.items and (len(self.items) >= self.max_entries or self.used + cost > self.max_bytes):
                    _, (_, old_cost, _) = self.items.popitem(last=False)
                    self.used -= old_cost
                    self.evictions += 1
                self.items[text] = (deepcopy(tree), cost, now + self.ttl)
                self.used += cost
            return tree

    def configure(self, enabled, clear=False):
        if type(enabled) is not bool or type(clear) is not bool:
            raise ValueError('Cache controls require booleans')
        with self.lock:
            self.enabled = enabled
            if clear or not enabled:
                self.items.clear()
                self.used = 0
            return self.stats()

    def stats(self):
        with self.lock:
            return dict(enabled=self.enabled, entries=len(self.items), accounted_bytes=self.used,
                        max_entries=self.max_entries, max_accounted_bytes=self.max_bytes,
                        hits=self.hits, misses=self.misses, evictions=self.evictions,
                        bypasses=self.bypasses, ttl_seconds=self.ttl,
                        expiration='lazy on subsequent cache requests; clear explicitly for immediate eviction',
                        scope='this Python parser module/process; syntax only, never results or parameter values')
