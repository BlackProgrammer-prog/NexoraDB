"""Read-only, startup-only resource policy; never disables durability/checksums."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re

MIB = 1024**2


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def _unescape(value):
    return re.sub(r'\\([0-7]{3})', lambda match: chr(int(match[1], 8)), value)


def cgroup_directories(proc=Path('/proc'), read_text=read):
    """Resolve v2 membership against mount roots, then include visible parents."""
    membership = read_text(proc / 'self/cgroup') or ''
    group = next((row[3:] for row in membership.splitlines() if row.startswith('0::')), None)
    if group is None:
        return []
    for row in (read_text(proc / 'self/mountinfo') or '').splitlines():
        fields = row.split()
        if ' - cgroup2 ' not in row or len(fields) < 7:
            continue
        root, mount = Path(_unescape(fields[3])), Path(_unescape(fields[4]))
        try:
            relative = Path(group).relative_to(root)
        except ValueError:
            continue
        if '..' in relative.parts:
            continue
        current = mount / relative
        return [current, *[p for p in current.parents if p == mount or mount in p.parents]]
    return []


def detect(db_path, *, proc=Path('/proc'), sys_root=Path('/sys'), read_text=read, affinity=None):
    if affinity is None:
        try:
            affinity = sorted(os.sched_getaffinity(0))
        except (AttributeError, OSError):
            affinity = list(range(os.cpu_count() or 1))
    cpus = max(1, len(affinity))
    limits, headrooms, quotas = [], [], []
    info = {}
    for row in (read_text(proc / 'meminfo') or '').splitlines():
        parts = row.split()
        if len(parts) >= 3 and parts[0] in ('MemTotal:', 'MemAvailable:') and parts[1].isdigit():
            info[parts[0][:-1]] = int(parts[1]) * 1024
    if info.get('MemTotal'):
        limits.append(info['MemTotal'])
    if info.get('MemAvailable') is not None:
        headrooms.append(info['MemAvailable'])
    groups = cgroup_directories(proc, read_text)
    for directory in groups:
        quota = (read_text(directory / 'cpu.max') or '').split()
        if len(quota) == 2 and quota[0].isdigit() and quota[1].isdigit() and int(quota[1]) > 0 and int(quota[0]) > 0:
            quotas.append(int(quota[0]) / int(quota[1]))
        current = read_text(directory / 'memory.current')
        for name in ('memory.high', 'memory.max'):
            value = read_text(directory / name)
            if value and value.isdigit():
                limit = int(value)
                limits.append(limit)
                if current and current.isdigit():
                    headrooms.append(max(0, limit - int(current)))
    if quotas:
        cpus = min(cpus, max(1, math.floor(min(quotas))))
    # Do not infer SSD from a VM name or filesystem. Resolve the actual device.
    disk = 'unknown'
    path = Path(db_path).absolute()
    while not path.exists() and path != path.parent:
        path = path.parent
    try:
        device = path.stat().st_dev
        node = sys_root / 'dev/block' / f'{os.major(device)}:{os.minor(device)}'
        if node.exists():
            resolved = node.resolve()
            for parent in (resolved, *resolved.parents):
                value = read_text(parent / 'queue/rotational')
                if value in ('0', '1'):
                    disk = 'rotational' if value == '1' else 'ssd'
                    break
    except (OSError, AttributeError):
        pass
    return {'affinity_cpus': list(affinity), 'effective_cpu_threads': cpus,
            'cpu_quota_cores': min(quotas) if quotas else None,
            'memory_limit_bytes': min(limits) if limits else None,
            'memory_headroom_bytes': min(headrooms) if headrooms else None,
            'cgroup_v2_visible_paths': [str(p) for p in groups], 'disk_class': disk,
            'scope': 'visible host/VM and cgroup v2 ancestors only; v1/hidden ancestors and physical Windows storage are not inferred'}


def recommend(observed, memory_mb=256, cache_mb=0):
    if type(memory_mb) is not int or not 4 <= memory_mb <= 16384 or type(cache_mb) is not int or not 0 <= cache_mb < memory_mb:
        raise ValueError('Invalid configured RocksDB budget')
    candidates = [memory_mb * MIB]
    if observed['memory_limit_bytes'] is not None:
        candidates.append(observed['memory_limit_bytes'] // 4)
    if observed['memory_headroom_bytes'] is not None:
        candidates.append(observed['memory_headroom_bytes'] // 2)
    budget = min(candidates) // MIB * MIB
    if budget < 4 * MIB:
        raise RuntimeError('Insufficient observed memory headroom for minimum engine budget')
    cache = cache_mb * MIB if cache_mb else budget // 2
    if cache < MIB or budget - cache < MIB:
        raise RuntimeError('Explicit cache conflicts with hardware budget; lower cache or disable auto tuning explicitly')
    return {'policy_version': 1, 'memory_budget_bytes': budget, 'block_cache_bytes': cache,
            'cpu_threads': observed['effective_cpu_threads'], 'disk_class': observed['disk_class'],
            'max_background_jobs': min(observed['effective_cpu_threads'], 4 if observed['disk_class'] == 'ssd' else 2),
            'scope': 'cache+memtable budget only, not process RSS; never expands configured memory; no runtime retuning'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--db-path', type=Path, default=Path('./nexoradb-data'))
    parser.add_argument('--memory-mb', type=int, default=256)
    parser.add_argument('--cache-mb', type=int, default=0)
    args = parser.parse_args()
    observed = detect(args.db_path)
    print(json.dumps({'observed': observed, 'recommended': recommend(observed, args.memory_mb, args.cache_mb)}, indent=2))


if __name__ == '__main__':
    main()
