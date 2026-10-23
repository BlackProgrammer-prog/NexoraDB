"""Linux process-tree sampling with explicit missing-metric reporting."""
import json
import os
import platform
import subprocess
import threading
import time
from datetime import datetime, timezone
from pathlib import Path


def process(pid):
    path = Path('/proc') / str(pid)
    stat = (path / 'stat').read_text().rsplit(')', 1)[1].split()
    status = {}
    for line in (path / 'status').read_text().splitlines():
        if line.startswith(('VmRSS:', 'VmHWM:')):
            key, value, *_ = line.split()
            status[key.rstrip(':')] = int(value) * 1024
    result = {'pid': pid, 'parent': int(stat[1]), 'start_ticks': int(stat[19]),
              'cpu_seconds': (int(stat[11]) + int(stat[12])) / os.sysconf('SC_CLK_TCK'),
              'rss_bytes': status.get('VmRSS', 0), 'hwm_bytes': status.get('VmHWM', 0)}
    try:
        io = dict(line.split(':', 1) for line in (path / 'io').read_text().splitlines())
        result.update(read_bytes=int(io['read_bytes']), write_bytes=int(io['write_bytes']))
    except PermissionError:
        result['io_error'] = 'permission_denied'
    try:
        for line in (path / 'smaps_rollup').read_text().splitlines():
            if line.startswith('Pss:'):
                result['pss_bytes'] = int(line.split()[1]) * 1024
    except (PermissionError, FileNotFoundError, ProcessLookupError):
        result['pss_error'] = 'unavailable'
    return result


def tree(roots):
    all_processes = {}
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            stat = path.read_text().rsplit(')', 1)[1].split()
            all_processes[int(path.parent.name)] = int(stat[1])
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    selected = set(roots)
    while True:
        expanded = selected | {pid for pid, parent in all_processes.items() if parent in selected}
        if expanded == selected:
            return sorted(selected)
        selected = expanded


def host_metadata(repo):
    def command(args):
        result = subprocess.run(args, cwd=repo, capture_output=True, text=True, check=False)
        return result.stdout.strip() if result.returncode == 0 else None
    return {'os': platform.platform(), 'kernel': platform.release(), 'python': platform.python_version(),
            'logical_cores': os.cpu_count(), 'affinity': sorted(os.sched_getaffinity(0)),
            'cpu': command(['lscpu', '--json']), 'memory': Path('/proc/meminfo').read_text(),
            'storage': command(['lsblk', '--json', '-o', 'NAME,TYPE,SIZE,ROTA,MODEL,MOUNTPOINTS']),
            'mounts': command(['findmnt', '--json', '--target', str(repo)]),
            'commit': command(['git', 'rev-parse', 'HEAD']),
            'worktree_status': command(['git', 'status', '--short']),
            'collected_utc': datetime.now(timezone.utc).isoformat()}


class Sampler:
    def __init__(self, path, roots, interval=1, combined=False):
        self.path, self.roots, self.interval = Path(path), roots, interval
        self.combined = combined
        self.stop = threading.Event()
        self.previous = {}
        self.deltas = {'server': {'cpu_seconds': 0, 'read_bytes': 0, 'write_bytes': 0},
                       'generator': {'cpu_seconds': 0, 'read_bytes': 0, 'write_bytes': 0}}
        self.peaks = {'server': {'rss_bytes': 0, 'pss_bytes': 0},
                      'generator': {'rss_bytes': 0, 'pss_bytes': 0}}
        self.errors = set()
        self.samples = 0
        self.deployment_peaks = {'rss_bytes': 0, 'pss_bytes': 0}
        self.progress_callback = None

    def sample(self):
        sample_cpu_start = time.thread_time()
        groups = {'server': tree(self.roots), 'generator': [os.getpid()]}
        data = {'monotonic_seconds': time.monotonic(), 'groups': {}}
        from .memory_diagnostics import python_memory, reclaim_allocator
        if self.combined:
            data['allocator_maintenance'] = reclaim_allocator()
        data['generator_allocator'] = python_memory()
        from .memory_control import host_memory, group_snapshot
        data['host_memory'] = host_memory()
        data['memory_containment'] = {name: group_snapshot(name) for name in ('postgresql', 'worker')}
        for group, pids in groups.items():
            rows = []
            for pid in pids:
                try:
                    row = process(pid)
                except (FileNotFoundError, ProcessLookupError):
                    continue
                except PermissionError:
                    self.errors.add(group + ':proc_permission_denied')
                    continue
                rows.append(row)
                for label in ('io_error', 'pss_error'):
                    if label in row:
                        self.errors.add(group + ':' + row[label])
                identity = (group, pid, row['start_ticks'])
                previous = self.previous.get(identity)
                if previous is not None:
                    for key in self.deltas[group]:
                        if key in row and key in previous:
                            self.deltas[group][key] += max(0, row[key] - previous[key])
                # New server children born during the window started at zero.
                elif self.samples:
                    for key in self.deltas[group]:
                        self.deltas[group][key] += row.get(key, 0)
                self.previous[identity] = row
            if not rows:
                self.errors.add(group + ':no_process_samples')
            sums = {key: sum(r.get(key, 0) for r in rows) for key in ('rss_bytes', 'pss_bytes')}
            for key, value in sums.items():
                self.peaks[group][key] = max(self.peaks[group][key], value)
            data['groups'][group] = {'processes': rows, 'memory_sum': sums}
        totals = {key: data['groups']['server']['memory_sum'][key]
                  + (0 if self.combined else data['groups']['generator']['memory_sum'][key])
                  for key in self.deployment_peaks}
        for key, value in totals.items():
            self.deployment_peaks[key] = max(self.deployment_peaks[key], value)
        data['deployment_memory'] = totals
        self.output.write(json.dumps(data) + '\n')
        self.output.flush()
        self.samples += 1
        self.sampler_cpu_seconds += time.thread_time() - sample_cpu_start

    def loop(self):
        while not self.stop.wait(self.interval):
            try:
                self.sample()
                if self.progress_callback is not None and time.monotonic() - self.last_progress >= 5:
                    before = time.thread_time()
                    self.progress_callback()
                    self.sampler_cpu_seconds += time.thread_time() - before
                    self.last_progress = time.monotonic()
            except Exception as error:
                self.errors.add('sampler:' + type(error).__name__)
                return

    def __enter__(self):
        self.output = self.path.open('x', encoding='utf-8')
        self.started = time.monotonic()
        self.last_progress = self.started
        self.sampler_cpu_seconds = 0
        self.sample()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join()
        try:
            self.sample()
        finally:
            self.output.close()
            self.ended = time.monotonic()

    def summary(self):
        elapsed = self.ended - self.started
        return {'sample_count': self.samples, 'sample_interval_seconds': self.interval,
                'window_seconds': elapsed, 'deltas': self.deltas, 'sampled_peaks': self.peaks,
                'sampler_cpu_seconds': self.sampler_cpu_seconds,
                'deployment_sampled_peaks': self.deployment_peaks,
                'cpu_percent_one_core': {g: d['cpu_seconds'] / elapsed * 100 for g, d in self.deltas.items()},
                'missing_metrics': sorted(self.errors),
                'attribution': 'native engine+generator combined' if self.combined else 'separate server tree and generator',
                'rss_caveat': 'PostgreSQL shared pages are counted per process; use aggregate PSS for comparisons.',
                'network': 'NOT_SUPPORTED: per-engine byte attribution unavailable for local TCP/embedded comparison',
                'process_exit_caveat': 'Short-lived processes between samples can escape sampling; no claim of exact peak.'}
