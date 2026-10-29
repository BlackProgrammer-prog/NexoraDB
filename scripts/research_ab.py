#!/usr/bin/env python3
"""Immutable, bounded A/B/A orchestration. Drivers emit one JSON object on stdout."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import signal
import shutil
import statistics
import subprocess
import tarfile
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def json_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, allow_nan=False).encode()).hexdigest()


def source_identity():
    def git(*args):
        # Root may run only the scoped cgroup controller in a user-owned repo.
        # Trust this exact workspace for this command, never change global Git config.
        return subprocess.check_output(['git', '-c', 'safe.directory=' + str(ROOT), '-C', str(ROOT), *args])
    paths = git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split(b'\0')
    # Source/config only. Large DB files, output, wheels and native binaries are
    # not a source tree; binaries are explicitly hashed independently below.
    files = {}
    for raw in sorted(set(paths)):
        path = ROOT / os.fsdecode(raw)
        if path.suffix.lower() in {'.py', '.cpp', '.h', '.hpp', '.cmake', '.json', '.toml', '.yml', '.yaml', '.lark', '.sh', '.txt', '.md', '.ts', '.tsx', '.html', '.css'} and path.is_file():
            files[os.fsdecode(raw)] = digest(path)
    return {'commit': git('rev-parse', 'HEAD').decode().strip(),
            'dirty': bool(git('status', '--porcelain').strip()),
            'source_files': files, 'source_sha256': json_hash(files)}


def host_identity():
    cpu = Path('/proc/cpuinfo').read_text() if Path('/proc/cpuinfo').exists() else ''
    model = next((line.split(':', 1)[1].strip() for line in cpu.splitlines() if line.startswith('model name')), platform.machine())
    return {'host_sha256': hashlib.sha256(platform.node().encode()).hexdigest(),
            'platform': platform.platform(), 'cpu': model,
            'affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
            'memory': Path('/proc/meminfo').read_text() if Path('/proc/meminfo').exists() else None,
            'allocator_env': {key: os.environ.get(key) for key in ('MALLOC_ARENA_MAX', 'MALLOC_TRIM_THRESHOLD_')}}


def archive_sources(source, destination):
    """Capture actual dirty source bytes, not only a commit name."""
    with tarfile.open(destination, 'x:gz') as archive:
        for name, sha in source.get('source_files', {}).items():
            path = ROOT / name
            if digest(path) != sha:
                raise ValueError('Source changed before archive: ' + name)
            archive.add(path, arcname=name, recursive=False)


def verify_loaded(sample, expected):
    if not isinstance(sample.get('loaded_artifacts'), dict) or sample['loaded_artifacts'] != expected:
        raise ValueError('Loaded artifacts differ from declared frozen binaries')


def bootstrap_median_interval(values, seed, draws=5000):
    """Percentile CI over independent block ratios, never individual requests."""
    if not values or any(not math.isfinite(x) or x <= 0 for x in values):
        raise ValueError('Finite positive block ratios required')
    rng = random.Random(seed)
    medians = sorted(statistics.median(rng.choices(values, k=len(values))) for _ in range(draws))
    return [medians[int(.025 * draws)], medians[min(draws - 1, int(.975 * draws))]]


def plan(config):
    required = {'schema', 'seed', 'repetitions', 'timeout_seconds', 'drift_fraction', 'max_cv', 'commands', 'binaries', 'workload'}
    if not required <= set(config) or set(config) - required - {'require_loaded_artifacts', 'affinity_cpus', 'telemetry_interval_seconds'} or config['schema'] != 1:
        raise ValueError('Unknown/missing config fields or schema')
    if type(config.get('require_loaded_artifacts', False)) is not bool:
        raise ValueError('require_loaded_artifacts must be boolean')
    cpus = config.get('affinity_cpus')
    if cpus is not None and (not isinstance(cpus, list) or not cpus or
            any(type(cpu) is not int or cpu < 0 for cpu in cpus) or len(set(cpus)) != len(cpus)):
        raise ValueError('affinity_cpus must be unique nonnegative integer IDs')
    interval = config.get('telemetry_interval_seconds', 1.)
    if type(interval) not in (int, float) or not math.isfinite(interval) or not .1 <= interval <= 10:
        raise ValueError('telemetry interval must be .1..10 seconds')
    if type(config['seed']) is not int or type(config['repetitions']) is not int or not 3 <= config['repetitions'] <= 20:
        raise ValueError('seed must be integer; repetitions must be 3..20')
    for name, upper in [('timeout_seconds', 600), ('drift_fraction', .5), ('max_cv', .5)]:
        value = config[name]
        if type(value) not in (int, float) or not math.isfinite(value) or not 0 < value <= upper:
            raise ValueError('Invalid ' + name)
    if set(config['commands']) != {'A', 'B'} or set(config['binaries']) != {'A', 'B'}:
        raise ValueError('Exactly A and B commands/binary lists required')
    for label in ('A', 'B'):
        command = config['commands'][label]
        if not isinstance(command, list) or not command or not all(isinstance(x, str) and x for x in command):
            raise ValueError('Commands must be nonempty argv arrays; no shell expansion')
        if not isinstance(config['binaries'][label], list) or not config['binaries'][label]:
            raise ValueError('Each arm needs explicit binary paths')
    if not isinstance(config['workload'], dict) or not config['workload']:
        raise ValueError('Explicit common workload contract required')
    rng = random.Random(config['seed'])
    order = []
    for block in range(config['repetitions']):
        first = rng.choice(['A', 'B'])
        order.extend({'block': block, 'position': position, 'arm': arm}
                     for position, arm in enumerate([first, 'B' if first == 'A' else 'A', first]))
    return {'config_sha256': json_hash(config), 'workload_sha256': json_hash(config['workload']),
            'order': order, 'maximum_seconds': len(order) * config['timeout_seconds'],
            'scope': 'short bounded experiments only; no publication certification'}


def validate_sample(sample, workload_hash):
    if sample.get('correct') is not True or sample.get('errors') != 0:
        raise ValueError('Driver did not establish correctness')
    if sample.get('workload_sha256') != workload_hash:
        raise ValueError('Driver workload differs from preregistered contract')
    if type(sample.get('operations')) is not int or sample['operations'] <= 0:
        raise ValueError('Positive operation count required')
    for key in ('ops_s', 'p99_us'):
        value = sample.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError('Invalid metric ' + key)
    if not sample.get('resource_scope'):
        raise ValueError('Explicit resource attribution required')


def exclusive_json(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def run(config, directory):
    if os.name != 'posix':
        raise ValueError('Run on Linux/WSL for isolated process-group cleanup')
    scheduled = plan(config)
    cpus = config.get('affinity_cpus')
    if cpus is not None and not set(cpus) <= os.sched_getaffinity(0):
        raise ValueError('Requested CPUs are outside allowed affinity')
    directory = Path(directory).resolve()
    if directory.is_relative_to(ROOT):
        ignored = subprocess.run(['git', '-c', 'safe.directory=' + str(ROOT), '-C', str(ROOT), 'check-ignore', '-q', str(directory)], check=False)
        if ignored.returncode:
            raise ValueError('Results inside repository must be git-ignored (for example build/research-...)')
    directory.mkdir(parents=True, exist_ok=False)
    binaries = {label: {str(Path(p).resolve()): digest(p) for p in paths}
                for label, paths in config['binaries'].items()}
    (directory / 'binaries').mkdir()
    binary_archive = {}
    for arm, files in binaries.items():
        binary_archive[arm] = {}
        for path, sha in files.items():
            relative = 'binaries/' + sha + '/' + Path(path).name
            target = directory / relative
            target.parent.mkdir(exist_ok=True)
            if not target.exists():
                shutil.copyfile(path, target)
            if digest(target) != sha:
                raise ValueError('Binary changed during snapshot')
            binary_archive[arm][path] = relative
    source = source_identity()
    archive_sources(source, directory / 'source.tar.gz')
    exclusive_json(directory / 'manifest.json', {'schema': 1, 'config': config,
                   'plan': scheduled, 'source': source, 'binaries': binaries,
                   'binary_archive': binary_archive,
                   'host': host_identity(), 'source_archive_sha256': digest(directory / 'source.tar.gz'),
                   'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())})
    for index, entry in enumerate(scheduled['order']):
        print(json.dumps({'event': 'sample-start', 'sample': index + 1,
                          'total': len(scheduled['order']), 'arm': entry['arm'],
                          'clients': config['workload'].get('clients'),
                          'timeout_seconds': config['timeout_seconds']}), file=sys.stderr, flush=True)
        if source_identity()['source_sha256'] != source['source_sha256']:
            raise ValueError('Source changed during experiment')
        if any(digest(path) != sha for values in binaries.values() for path, sha in values.items()):
            raise ValueError('Binary changed during experiment')
        env = os.environ.copy()
        env['NEXORA_RESEARCH_WORKLOAD'] = json.dumps(config['workload'], sort_keys=True)
        env['NEXORA_RESEARCH_SEED'] = str(config['seed'])
        native_paths = [path for path in binaries[entry['arm']] if Path(path).name.startswith('nexoradb') and path.endswith('.so')]
        if len(native_paths) == 1:
            env['NEXORA_RESEARCH_NATIVE_PATH'] = native_paths[0]
        if cpus is not None:
            # Native driver sets affinity before opening any engine threads.
            env['NEXORA_RESEARCH_CPUS'] = json.dumps(cpus)
        started = time.monotonic()
        from research_telemetry import Telemetry
        with (directory / f'{index:03d}.stdout').open('x') as stdout, (directory / f'{index:03d}.stderr').open('x') as stderr:
            child = subprocess.Popen(config['commands'][entry['arm']], cwd=ROOT,
                                     env=env, stdout=stdout, stderr=stderr, start_new_session=True)
            try:
                with Telemetry(directory / f'{index:03d}.telemetry.jsonl', cpus or sorted(os.sched_getaffinity(0))[:4],
                               config.get('telemetry_interval_seconds', 1.)):
                    code = child.wait(timeout=config['timeout_seconds'])
            finally:
                # Only the group created by this invocation; also reap descendants.
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
        if code:
            raise ValueError(f'Driver failed: {index}; inspect retained stdout/stderr')
        if source_identity()['source_sha256'] != source['source_sha256'] or any(
                digest(path) != sha for values in binaries.values() for path, sha in values.items()):
            raise ValueError('Source or binary changed during sample')
        sample = json.loads((directory / f'{index:03d}.stdout').read_text())
        validate_sample(sample, scheduled['workload_sha256'])
        if 'operations' in config['workload'] and sample['operations'] != config['workload']['operations']:
            raise ValueError('Driver operation count differs from the fixed-count workload')
        if cpus is not None and sample.get('effective_affinity') != sorted(cpus):
            raise ValueError('Driver did not apply the declared CPU affinity')
        if config.get('require_loaded_artifacts', False):
            verify_loaded(sample, binaries[entry['arm']])
        exclusive_json(directory / f'{index:03d}.json', {**entry, 'sample': sample,
                       'wall_seconds_including_startup': time.monotonic() - started})
        print(json.dumps({'event': 'sample-complete', 'sample': index + 1,
                          'ops_s': sample['ops_s'], 'p99_us': sample['p99_us']}), file=sys.stderr, flush=True)
    exclusive_json(directory / 'completed.json', {'samples': len(scheduled['order']),
                   'file_sha256': {str(p.relative_to(directory)): digest(p) for p in sorted(directory.rglob('*')) if p.is_file()}})
    return analyze(directory)


def analyze(directory):
    directory = Path(directory)
    manifest = json.loads((directory / 'manifest.json').read_text())
    scheduled = plan(manifest['config'])
    if manifest['plan'] != scheduled or not (directory / 'completed.json').exists():
        raise ValueError('Incomplete experiment or modified plan')
    completion = json.loads((directory / 'completed.json').read_text())
    if completion['samples'] != len(scheduled['order']):
        raise ValueError('Invalid completion count')
    required_evidence = {'manifest.json'} | {f'{i:03d}.json' for i in range(len(scheduled['order']))}
    if not required_evidence <= set(completion['file_sha256']):
        raise ValueError('Completion lacks required evidence hashes')
    for name, sha in completion['file_sha256'].items():
        if Path(name).is_absolute() or '..' in Path(name).parts or not (directory / name).resolve().is_relative_to(directory.resolve()) or digest(directory / name) != sha:
            raise ValueError('Experiment evidence changed: ' + name)
    rows = [json.loads((directory / f'{i:03d}.json').read_text()) for i in range(len(scheduled['order']))]
    ratios, latency_ratios, drift = [], [], []
    for row, entry in zip(rows, scheduled['order']):
        if any(row[key] != entry[key] for key in entry):
            raise ValueError('Sample ordering mismatch')
        validate_sample(row['sample'], scheduled['workload_sha256'])
        if 'operations' in manifest['config']['workload'] and row['sample']['operations'] != manifest['config']['workload']['operations']:
            raise ValueError('Recorded operation count differs from the workload')
        if manifest['config'].get('require_loaded_artifacts', False):
            verify_loaded(row['sample'], manifest['binaries'][row['arm']])
    if len({row['sample']['operations'] for row in rows}) != 1:
        raise ValueError('Operation counts differ')
    runtimes = {json_hash(row['sample']['driver_runtime']) for row in rows if row['sample'].get('driver_runtime') is not None}
    if len(runtimes) > 1:
        raise ValueError('Driver/interpreter dependency environment changed across arms')
    for offset in range(0, len(rows), 3):
        first, middle, last = rows[offset:offset + 3]
        ends = [first['sample'], last['sample']]
        drift.append(max(abs(ends[1][key] / ends[0][key] - 1) for key in ('ops_s', 'p99_us')))
        arms = {first['arm']: {key: statistics.mean(s[key] for s in ends) for key in ('ops_s', 'p99_us')},
                middle['arm']: middle['sample']}
        ratios.append(arms['B']['ops_s'] / arms['A']['ops_s'])
        latency_ratios.append(arms['B']['p99_us'] / arms['A']['p99_us'])
    variability = {}
    for arm in ('A', 'B'):
        values = [row['sample']['ops_s'] for row in rows if row['arm'] == arm]
        variability[arm] = statistics.stdev(values) / statistics.mean(values)
    stable = max(drift) <= manifest['config']['drift_fraction'] and max(variability.values()) <= manifest['config']['max_cv']
    return {'schema': 1, 'classification': 'descriptive-stable' if stable else 'inconclusive-environment-drift',
            'publication_eligible': False, 'blocks': len(ratios), 'max_bracket_drift': max(drift),
            'throughput_cv': variability, 'B_over_A_ops_median': statistics.median(ratios),
            'B_over_A_ops_range': [min(ratios), max(ratios)],
            'B_over_A_p99_median': statistics.median(latency_ratios),
            'B_over_A_ops_ci95': bootstrap_median_interval(ratios, manifest['config']['seed']),
            'B_over_A_p99_ci95': bootstrap_median_interval(latency_ratios, manifest['config']['seed'] + 1),
            'inference_minimum_blocks_met': len(ratios) >= 5,
            'driver_runtime_recorded': len(runtimes) == 1,
            'bootstrap': {'method': 'percentile median over paired bracket blocks', 'draws': 5000, 'seed': manifest['config']['seed']},
            'limitations': 'CI is exploratory below five independent blocks; bracket drift covers throughput and p99. Driver oracle and build provenance require audit. No automatic winner.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['plan', 'run', 'analyze'])
    parser.add_argument('--config', type=Path)
    parser.add_argument('--directory', type=Path)
    args = parser.parse_args()
    if args.action in ('plan', 'run') and args.config is None:
        parser.error('--config required')
    if args.action in ('run', 'analyze') and args.directory is None:
        parser.error('--directory required')
    config = json.loads(args.config.read_text()) if args.config else None
    value = plan(config) if args.action == 'plan' else run(config, args.directory) if args.action == 'run' else analyze(args.directory)
    print(json.dumps(value, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
