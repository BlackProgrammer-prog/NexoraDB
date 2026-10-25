"""Standard-library terminal dashboard and machine-readable stage heartbeat."""
import json
import shutil
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path


def duration(seconds):
    if seconds is None:
        return '--'
    seconds = max(0, int(seconds))
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    return f'{hours:02d}:{minutes:02d}:{seconds:02d}'


def bar(current, total, width=28):
    if total is None or total < 0:
        return '[' + '.' * width + ']  --%'
    fraction = 1 if total == 0 else max(0, min(1, current / total))
    filled = int(fraction * width)
    return '[' + '#' * filled + '-' * (width - filled) + f'] {fraction * 100:5.1f}%'


class WorkerProgress:
    def __init__(self, directory, write, interval=1):
        self.path = Path(directory) / 'progress.json'
        self.write, self.interval = write, interval
        self.lock = threading.Lock()
        self.payload = {}

    def start(self, stage, total=None, unit='records', **details):
        with self.lock:
            self.started = time.monotonic()
            self.last_write = 0
            self.previous_current = 0
            self.previous_time = self.started
            self.payload = {'stage': stage, 'total': total, 'unit': unit, **details}
            self._update(0, True, {})

    def update(self, current, force=False, **details):
        with self.lock:
            self._update(current, force, details)

    def _update(self, current, force, details):
        now = time.monotonic()
        total = self.payload['total']
        self.payload.update(details)
        if not force and now - self.last_write < self.interval and (total is None or current < total):
            return
        elapsed = now - self.started
        average = current / elapsed if elapsed > 0 else 0
        recent = max(0, current - self.previous_current) / max(1e-9, now - self.previous_time)
        self.payload.update(current=current, elapsed_seconds=elapsed,
                            average_per_second=average, recent_per_second=recent,
                            eta_seconds=(max(0, total - current) / average
                                         if total is not None and average > 0 else None),
                            updated_utc=datetime.now(timezone.utc).isoformat())
        if self.payload['unit'] == 'seconds' and total is not None:
            self.payload['eta_seconds'] = (None if self.payload.get('details', {}).get('draining')
                                           else max(0, total - current))
        self.write(self.path, dict(self.payload))
        self.previous_current, self.previous_time = current, now
        self.last_write = now


class Dashboard:
    """TTY refreshes in place; redirected output remains plain JSONL."""
    def __init__(self, directory, stream=None, tty=None):
        self.directory = Path(directory)
        self.stream = stream if stream is not None else sys.stderr
        self.tty = self.stream.isatty() if tty is None else tty
        self.lines = 0
        self.last_event = None
        self.last_json = 0
        self.started = time.monotonic()
        self.events = (self.directory / 'events.jsonl').open('a', encoding='utf-8')

    def clear(self):
        if self.tty and self.lines:
            self.stream.write(f'\x1b[{self.lines}A\x1b[J')
            self.lines = 0

    def event(self, payload):
        entry = {'utc': datetime.now(timezone.utc).isoformat(), **payload}
        self.events.write(json.dumps(entry) + '\n')
        self.events.flush()
        self.clear()
        if self.tty:
            label = payload.get('stage', payload.get('event', 'event'))
            self.stream.write(f'[{entry["utc"][11:19]} UTC] {payload.get("database", "")} {label}\n')
            self.stream.flush()
        else:
            print(json.dumps(entry), flush=True)

    def update(self, snapshot):
        key = (snapshot.get('job'), snapshot.get('stage'), snapshot.get('cell'))
        now = time.monotonic()
        if key != self.last_event:
            self.event({'event': 'stage', **snapshot})
            self.last_event = key
        elif now - self.last_json >= 5:
            entry = {'event': 'progress', **snapshot}
            self.events.write(json.dumps({'utc': datetime.now(timezone.utc).isoformat(), **entry}) + '\n')
            self.events.flush()
            if not self.tty:
                print(json.dumps(entry), flush=True)
            self.last_json = now
        if self.tty:
            self.clear()
            lines = self.render(snapshot)
            columns = max(35, shutil.get_terminal_size((100, 24)).columns - 1)
            lines = [line[:columns] for line in lines]
            self.stream.write('\n'.join(lines) + '\n')
            self.stream.flush()
            self.lines = len(lines)

    def render(self, value):
        current, total = value.get('current', 0), value.get('total')
        stage = value.get('stage', 'starting')
        if stage == 'counting-records' and value.get('next_stage'):
            stage += ' -> ' + value['next_stage']
        unit = value.get('unit', 'records')
        completed = value.get('completed_jobs', 0)
        jobs = value.get('total_jobs', 1)
        details = value.get('details', {})
        if details.get('draining'):
            stage += ' (waiting for in-flight requests)'
        count = f'{current:,.0f}' + (f' / {total:,.0f}' if total is not None else '')
        speed = value.get('average_per_second', 0)
        memory = value.get('host_memory', {})
        pg_memory = value.get('postgresql_memory') or {}
        gib = 1024**3
        if unit == 'seconds':
            rate = f'Operations: {details.get("attempted", 0):,} | failures: {details.get("failed", 0):,}'
        else:
            rate = f'Average: {speed:,.0f} {unit}/s | recent: {value.get("recent_per_second", 0):,.0f} {unit}/s'
        lines = [
            'NEXORA SCALE LAB  |  LIVE PROGRESS  |  Ctrl+C: stop safely',
            f'Jobs: {completed}/{jobs} completed  |  current: {value.get("database", "--")}  |  run elapsed: {duration(now_elapsed(self.started))}',
            f'Trials: {value.get("completed_trials", 0)}/{value.get("total_trials", 0)} verified  |  job {value.get("job", "--")}  |  trial {value.get("current_trial", "--")}',
            f'Stage: {stage}  |  workload: {value.get("workload", "--")}  |  clients: {value.get("clients", "--")}',
            f'{bar(current, total)}  {count} {unit}',
            rate,
            f'Linux available: {memory.get("MemAvailable", 0)/gib:.1f} GiB | PG group: {pg_memory.get("current_bytes", 0)/gib:.1f}/8 GiB (includes cache)',
            f'Stage elapsed: {duration(value.get("elapsed_seconds"))} | stage ETA: {duration(value.get("eta_seconds"))} | last counter update: {value.get("stale_seconds", 0):.0f}s ago',
            f'Logs/results: {self.directory}',
        ]
        metrics = value.get('engine_metrics') or {}
        if metrics:
            lines.insert(7, f'Rocks: L0={metrics.get("l0_files", 0)} | compaction debt={metrics.get("pending_compaction_bytes", 0)/1024**2:.0f} MiB | stalls={metrics.get("stall_micros", 0)/1e6:.2f}s')
            lines.insert(8, f'Memtables: {metrics.get("memtable_usage", 0)/1024**2:.0f} MiB | batch/ack={value.get("batch_ack_seconds", 0):.1f}s | producer wait={value.get("producer_wait_seconds", 0):.1f}s')
            write_path=metrics.get('write_path') or {}
            sync=write_path.get('wal_sync_latency') or {}
            if sync:
                lines.insert(9, f'WAL sync cumulative p99: {sync.get("p99_us", 0)/1000:.1f}ms | last batch ACK: {value.get("last_batch_ack_ms", 0):.1f}ms | syncs: {write_path.get("wal_syncs", 0)}')
        return lines

    def close(self):
        self.clear()
        self.events.close()


def now_elapsed(started):
    return time.monotonic() - started


def snapshot(attempt, job, completed_jobs, total_jobs, completed_trials, total_trials):
    attempt = Path(attempt)
    checkpoint_path = attempt / 'checkpoint.json'
    checkpoint = json.loads(checkpoint_path.read_text()) if checkpoint_path.exists() else {'status': 'starting'}
    live_path = attempt / 'progress.json'
    live = json.loads(live_path.read_text()) if live_path.exists() else {}
    if live.get('stage') != checkpoint['status']:
        live = {}
    verified = 0
    for path in attempt.glob('cell-*/measurement.json'):
        if json.loads(path.read_text()).get('status') == 'verified-and-restored':
            verified += 1
    stale = 0
    if live.get('updated_utc'):
        stale = max(0, (datetime.now(timezone.utc) - datetime.fromisoformat(live['updated_utc'])).total_seconds())
    result = {**live, 'job': job['id'], 'database': job['database'],
              'stage': checkpoint['status'], 'cell': checkpoint.get('cell'),
              'next_stage': checkpoint.get('next_stage'),
              'workload': checkpoint.get('workload', '--'), 'clients': checkpoint.get('clients', '--'),
              'completed_jobs': completed_jobs, 'total_jobs': total_jobs,
              'completed_trials': completed_trials + verified, 'total_trials': total_trials,
              'current_trial': (f'{checkpoint["cell"] + 1}/{len(job["cells"])}'
                                if checkpoint.get('cell') is not None else '--'),
              'stale_seconds': stale}
    from .memory_control import host_memory, group_snapshot
    result['host_memory'] = host_memory()
    result['postgresql_memory'] = group_snapshot('postgresql')
    if checkpoint.get('stage_started_utc'):
        result['elapsed_seconds'] = max(0, (datetime.now(timezone.utc)
            - datetime.fromisoformat(checkpoint['stage_started_utc'])).total_seconds())
    return result
