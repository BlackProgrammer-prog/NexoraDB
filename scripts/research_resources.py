"""Linux-only, sampled process accounting with explicit, non-overlapping scopes."""
import os
from pathlib import Path
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'nexora-scale-lab'))
from scale_lab.resources import process, tree


class ProcessSampler:
    def __init__(self, scopes, interval=.02):
        self.scopes, self.interval = scopes, interval
        roots = [pid for values in scopes.values() for pid in values]
        if len(roots) != len(set(roots)):
            raise ValueError('A process cannot belong to multiple resource scopes')
        self.identities = {pid: process(pid)['start_ticks'] for pid in roots}
        self.stop = threading.Event()
        self.peaks = {name: {'rss_peak_sampled_bytes': 0, 'pss_peak_sampled_bytes': 0,
                             'cpu_seconds_observed': 0., 'samples': 0, 'missing_pss_samples': 0}
                      for name in scopes}
        self.cpu = {}
        self.error = None

    def sample(self):
        owned = set()
        for name, roots in self.scopes.items():
            valid = []
            for pid in roots:
                try:
                    if process(pid)['start_ticks'] == self.identities[pid]:
                        valid.append(pid)
                except (FileNotFoundError, ProcessLookupError):
                    pass
            selected = set(tree(valid)) if valid else set()
            if selected & owned:
                raise ValueError('Overlapping process trees would double-count resources')
            owned.update(selected)
            rss = pss = 0
            row = self.peaks[name]
            for pid in selected:
                try:
                    value = process(pid)
                except (FileNotFoundError, ProcessLookupError):
                    continue
                identity = (name, pid, value['start_ticks'])
                previous = self.cpu.get(identity, value['cpu_seconds'])
                row['cpu_seconds_observed'] += max(0, value['cpu_seconds'] - previous)
                self.cpu[identity] = value['cpu_seconds']
                rss += value['rss_bytes']
                if 'pss_bytes' not in value:
                    row['missing_pss_samples'] += 1
                pss += value.get('pss_bytes', 0)
            row['rss_peak_sampled_bytes'] = max(row['rss_peak_sampled_bytes'], rss)
            row['pss_peak_sampled_bytes'] = max(row['pss_peak_sampled_bytes'], pss)
            row['samples'] += 1

    def loop(self):
        try:
            while not self.stop.wait(self.interval):
                self.sample()
        except Exception as exc:
            self.error = exc

    def __enter__(self):
        self.sample()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(timeout=5)
        if self.error:
            raise self.error

    def report(self):
        return {'scopes': self.peaks, 'interval_seconds': self.interval,
                'cpu_clock_tick_seconds': 1 / os.sysconf('SC_CLK_TCK'),
                'root_identities': self.identities,
                'limitations': 'Sampled lifecycle peaks, not exact component allocations. RSS shared pages may duplicate within a tree; prefer PSS. CPU excludes unsampled process lifetimes and startup before sampling. Do not add RSS to cgroup memory.'}
