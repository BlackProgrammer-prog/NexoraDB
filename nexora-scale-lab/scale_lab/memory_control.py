"""Scoped cgroup-v2 limits: anonymous RAM AND charged filesystem page cache.

Only the two explicitly named benchmark cgroups are modified. Kernel control
files are runtime APIs, not application source/config files.
"""
import json
import os
import sys
from contextlib import contextmanager
from pathlib import Path

ROOT = Path('/sys/fs/cgroup')
GROUPS = {'postgresql': 'nexora-scale-lab-postgresql', 'worker': 'nexora-scale-lab-worker'}
HIGH = 6 * 1024**3
MAXIMUM = 8 * 1024**3
SWAP = 1024**3


def ensure_group(name):
    if os.geteuid() != 0:
        raise RuntimeError('Memory containment requires root inside WSL')
    path = ROOT / GROUPS[name]
    if 'memory' not in (ROOT / 'cgroup.controllers').read_text().split():
        raise RuntimeError('cgroup-v2 memory controller unavailable; refusing an uncontained run')
    if 'memory' not in (ROOT / 'cgroup.subtree_control').read_text().split():
        (ROOT / 'cgroup.subtree_control').write_text('+memory')
    path.mkdir(exist_ok=True)
    for setting, value in {'memory.high': HIGH, 'memory.max': MAXIMUM,
                           'memory.swap.max': SWAP, 'memory.oom.group': 1}.items():
        (path / setting).write_text(str(value))
    if int((path / 'memory.max').read_text()) != MAXIMUM:
        raise RuntimeError('Kernel did not apply benchmark memory.max')
    return path


def enter_group(name):
    path = ensure_group(name)
    (path / 'cgroup.procs').write_text(str(os.getpid()))
    if str(path.relative_to(ROOT)) not in Path('/proc/self/cgroup').read_text():
        raise RuntimeError('Failed to enter benchmark memory cgroup')
    return group_snapshot(name)


def group_snapshot(name):
    path = ROOT / GROUPS[name]
    if not (path / 'memory.current').exists():
        return None
    stats = dict(line.split() for line in (path / 'memory.stat').read_text().splitlines())
    return {'path': str(path), 'current_bytes': int((path / 'memory.current').read_text()),
            'high_bytes': (path / 'memory.high').read_text().strip(),
            'max_bytes': (path / 'memory.max').read_text().strip(),
            'swap_max_bytes': (path / 'memory.swap.max').read_text().strip(),
            'swap_current_bytes': int((path / 'memory.swap.current').read_text()),
            'pressure': (path / 'memory.pressure').read_text().strip(),
            'anon_bytes': int(stats.get('anon', 0)), 'file_cache_bytes': int(stats.get('file', 0)),
            'dirty_bytes': int(stats.get('file_dirty', 0)),
            'events': dict((key, int(value)) for key, value in
                           (line.split() for line in (path / 'memory.events').read_text().splitlines()))}


def host_memory():
    values = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        fields = line.split()
        if fields[0] in {'MemTotal:', 'MemAvailable:', 'Cached:', 'Dirty:', 'Writeback:', 'SwapFree:'}:
            values[fields[0].rstrip(':')] = int(fields[1]) * 1024
    return values


@contextmanager
def cleanup_headroom(enabled):
    """Only after measurements: avoid reclaim thrashing while releasing memory.

    Hard RAM/swap caps are unchanged. Always restore the original soft threshold.
    The controller's experiment lock must remain held throughout this scope.
    """
    if not enabled:
        yield
        return
    path = ROOT / GROUPS['worker']
    membership = Path('/proc/self/cgroup').read_text()
    if '0::/' + GROUPS['worker'] not in membership.splitlines():
        raise RuntimeError('Cleanup headroom requires worker cgroup membership')
    previous = (path / 'memory.high').read_text().strip()
    maximum = (path / 'memory.max').read_text().strip()
    if maximum != str(MAXIMUM):
        raise RuntimeError('Unexpected hard cap; refusing cleanup limit change')
    try:
        (path / 'memory.high').write_text(maximum)
        yield
    finally:
        (path / 'memory.high').write_text(previous)


def assert_postgres_contained(pid):
    group = GROUPS['postgresql']
    if group not in Path(f'/proc/{pid}/cgroup').read_text():
        raise RuntimeError('PostgreSQL is outside its memory cgroup; restart via start-local-services.sh')
    return group_snapshot('postgresql')


if __name__ == '__main__':
    if sys.argv[1:] == ['postgres-start']:
        enter_group('postgresql')
        os.execv('/usr/bin/pg_ctlcluster', ['pg_ctlcluster', '--skip-systemctl-redirect', '14', 'main', 'start'])
    elif sys.argv[1:] == ['prepare']:
        print(json.dumps({name: (ensure_group(name), group_snapshot(name))[1] for name in GROUPS}, indent=2))
    else:
        raise SystemExit('Usage: python -m scale_lab.memory_control prepare|postgres-start')
