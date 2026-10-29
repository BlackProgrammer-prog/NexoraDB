#!/usr/bin/env python3
"""Run only a new smoke subprocess in an ephemeral cgroup; never move servers."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import uuid

CGROOT = Path('/sys/fs/cgroup')


def snapshot(path):
    stats = dict(line.split() for line in (path / 'memory.stat').read_text().splitlines())
    return {'current_bytes': int((path / 'memory.current').read_text()),
            'peak_bytes': int((path / 'memory.peak').read_text()),
            'high_bytes': int((path / 'memory.high').read_text()),
            'max_bytes': int((path / 'memory.max').read_text()),
            'swap_max_bytes': int((path / 'memory.swap.max').read_text()),
            'swap_current_bytes': int((path / 'memory.swap.current').read_text()),
            'anon_bytes': int(stats.get('anon', 0)), 'file_bytes': int(stats.get('file', 0)),
            'events': {key: int(value) for key, value in (line.split() for line in (path / 'memory.events').read_text().splitlines())},
            'pressure': (path / 'memory.pressure').read_text()}


def run(command, maximum=2 * 1024**3, high=1536 * 1024**2, timeout=60):
    if not command or not 0 < timeout <= 60:
        raise ValueError('Explicit command and timeout in (0,60] required')
    if os.geteuid() != 0 or not 0 < high <= maximum <= 2 * 1024**3:
        raise ValueError('Root and bounded 0<high<=max<=2GiB required')
    if 'memory' not in (CGROOT / 'cgroup.subtree_control').read_text().split():
        raise ValueError('Memory controller must already be enabled; parent settings are never changed')
    path = CGROOT / ('nexora-research-' + uuid.uuid4().hex)
    path.mkdir()
    child = None
    try:
        for name, value in {'memory.high': high, 'memory.max': maximum,
                            'memory.swap.max': 0, 'memory.oom.group': 1}.items():
            (path / name).write_text(str(value))
        before = snapshot(path)
        if before['max_bytes'] != maximum or before['high_bytes'] != high or before['swap_max_bytes'] != 0:
            raise ValueError('Kernel rejected resource limits')
        def enter():
            # Runs only in the brand-new child before exec, no existing PID migration.
            (path / 'cgroup.procs').write_text(str(os.getpid()))
        started = time.monotonic()
        with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
            child = subprocess.Popen(command, stdout=stdout, stderr=stderr, preexec_fn=enter, start_new_session=True)
            samples = []
            while child.poll() is None:
                if time.monotonic() - started > timeout:
                    raise TimeoutError('Bounded memory smoke timed out')
                samples.append(snapshot(path))
                time.sleep(.02)
            final = snapshot(path)
            stdout.seek(0)
            stderr.seek(0)
            if child.returncode:
                raise RuntimeError(f'Driver failed ({child.returncode}): {stderr.read().decode()[-4000:]}')
            result = json.loads(stdout.read())
        if ('0::/' + path.name) not in result.get('cgroup_membership', '').splitlines():
            raise ValueError('Driver did not confirm membership in its dedicated cgroup')
        changes = {key: final['events'][key] - before['events'][key] for key in final['events']}
        if any(changes.get(key, 0) for key in ('high', 'max', 'oom', 'oom_kill')):
            raise ValueError('Memory pressure/limit event invalidates this acceptance run: ' + json.dumps(changes))
        if result.get('correct') is not True or result.get('close_seconds', 999) > 10:
            raise ValueError('Correctness or close-time acceptance failed')
        result.update(memory_limit_enforced=True, cgroup={'before': before, 'after': final,
                      'event_deltas': changes, 'samples': samples,
                      'scope': 'new driver process + native engine + oracle + charged file cache; no separate generator sum'})
        return result
    finally:
        if child is not None:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
        # Kill only descendants still in this uniquely owned cgroup, then rmdir
        # the empty kernel object. No recursive filesystem removal is used.
        if (path / 'cgroup.kill').exists():
            (path / 'cgroup.kill').write_text('1')
        deadline = time.monotonic() + 5
        while (path / 'cgroup.procs').read_text().strip() and time.monotonic() < deadline:
            time.sleep(.02)
        path.rmdir()


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt('Scoped memory probe interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('Explicit subprocess argv required')
    print(json.dumps(run(command), allow_nan=False))


if __name__ == '__main__':
    main()
