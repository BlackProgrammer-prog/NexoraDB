"""Read-only host telemetry. Missing WSL counters are explicit, never guessed."""
import json
from pathlib import Path
import threading
import time


def read(path):
    try:
        return {'available': True, 'value': Path(path).read_text().strip()}
    except OSError as exc:
        return {'available': False, 'errno': exc.errno}


def snapshot(cpus):
    return {'monotonic_ns': time.monotonic_ns(),
            'pressure': {name: read('/proc/pressure/' + name) for name in ('cpu', 'io', 'memory')},
            'loadavg': read('/proc/loadavg'), 'diskstats': read('/proc/diskstats'),
            'meminfo': read('/proc/meminfo'),
            'cpu_policy': {str(cpu): {key: read(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/{key}')
                                     for key in ('scaling_cur_freq', 'scaling_governor')}
                           for cpu in cpus},
            'scope': 'host-global observations, not engine attribution; governor/frequency may be unavailable in WSL'}


class Telemetry:
    def __init__(self, path, cpus, interval=1.):
        self.path, self.cpus, self.interval = Path(path), cpus, interval
        self.stop = threading.Event()
        self.error = None

    def sample(self):
        self.stream.write(json.dumps(snapshot(self.cpus), allow_nan=False) + '\n')
        self.stream.flush()

    def loop(self):
        try:
            while not self.stop.wait(self.interval):
                self.sample()
        except Exception as exc:
            self.error = exc

    def __enter__(self):
        self.stream = self.path.open('x')
        self.sample()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(timeout=5)
        try:
            self.sample()
        finally:
            self.stream.close()
        if self.error:
            raise self.error
