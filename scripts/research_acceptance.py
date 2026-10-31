#!/usr/bin/env python3
"""Plan first; long run is explicit. Never edits the roadmap or releases artifacts."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

from research_ab import ROOT, analyze as analyze_ab, digest, exclusive_json, run as run_ab, source_identity
from research_layers import run as run_layers


def make_plan(profile, cpus=None):
    if profile not in ('short', 'long'):
        raise ValueError('profile must be short or long')
    allowed = sorted(os.sched_getaffinity(0))
    cpus = allowed[:min(4, len(allowed))] if cpus is None else sorted(cpus)
    if not cpus or len(set(cpus)) != len(cpus) or not set(cpus) <= set(allowed):
        raise ValueError('Select unique CPUs within current allowed affinity')
    long = profile == 'long'
    native = ROOT / 'NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so'
    cpp = ROOT / 'build/release/nexora_research_read_probe'
    cfg = json.loads((ROOT / 'nexora-scale-lab/config/research-smoke-v1.json').read_text())
    cfg.update(repetitions=5 if long else 3, timeout_seconds=600 if long else 60,
               affinity_cpus=cpus, telemetry_interval_seconds=1.)
    cfg['commands'] = {arm: [sys.executable, str(ROOT / 'scripts/research_workload.py')] for arm in ('A', 'B')}
    cfg['binaries'] = {arm: [str(native)] for arm in ('A', 'B')}
    cfg['workload'].update(version=3, records=10000 if long else 64,
                           operations=1_000_000 if long else 200,
                           warmup_operations=100000 if long else 100,
                           allow_long=long)
    configs = []
    for clients in (1, 10):
        value = json.loads(json.dumps(cfg))
        value['workload']['clients'] = clients
        configs.append(value)
    result = {'schema': 1, 'profile': profile, 'cpus': cpus, 'native_path': str(native), 'cpp_path': str(cpp),
              'aa_configs': configs, 'layer_repetitions': 5 if long else 1,
              'layer_records': 10000 if long else 64, 'layer_operations': 20000 if long else 100,
              'layer_warmup_operations': 10000 if long else 10, 'layer_timeout_seconds': 600 if long else 60,
              'thresholds': {'max_bracket_drift': .10, 'max_throughput_cv': .10, 'max_layer_p99_cv': .10},
              'hypotheses_accepted': [], 'publication_eligible': False}
    result['maximum_scheduled_seconds'] = sum(3 * cfg['repetitions'] * cfg['timeout_seconds'] for cfg in configs) + result['layer_repetitions'] * (3 * result['layer_timeout_seconds'] + 40)
    return result


def verify_seal(directory, required=('plan.json',)):
    directory = Path(directory).resolve()
    seal = json.loads((directory / 'completed.json').read_text())['sha256']
    for name, sha in seal.items():
        path = directory / name
        if Path(name).is_absolute() or '..' in Path(name).parts or not path.resolve().is_relative_to(directory) or digest(path) != sha:
            raise ValueError('Modified evidence: ' + name)
    if not set(required) <= set(seal):
        raise ValueError('Required metadata was not sealed')


def acceptance(aa, layers, plan):
    if plan['thresholds'] != {'max_bracket_drift': .10, 'max_throughput_cv': .10, 'max_layer_p99_cv': .10}:
        raise ValueError('Acceptance thresholds are frozen for this profile version')
    failures = []
    if plan['profile'] != 'long':
        failures.append('short profile validates wiring only; not sufficient for parent acceptance')
    if (len(plan['aa_configs']) != 2 or {cfg['workload']['clients'] for cfg in plan['aa_configs']} != {1, 10}
            or any(cfg['workload']['operations'] < 1_000_000 or cfg['workload']['warmup_operations'] < 100000
                   or cfg['repetitions'] < 5 for cfg in plan['aa_configs'])
            or plan['layer_operations'] < 20000 or plan['layer_warmup_operations'] < 10000):
        failures.append('long profile sample count/warm-up requirements not met')
    for clients, row in aa.items():
        if row['classification'] != 'descriptive-stable' or not row['inference_minimum_blocks_met'] or not row.get('driver_runtime_recorded', False):
            failures.append('A/A unstable or insufficient blocks, clients=' + clients)
        for metric in ('ops', 'p99'):
            low, high = row['B_over_A_' + metric + '_ci95']
            if not low <= 1 <= high:
                failures.append(f'A/A null control excludes ratio 1: clients={clients}, metric={metric}')
    if len(layers) < 5:
        failures.append('five independent layer repetitions required')
    variation = {}
    for name in ('cpp', 'binding', 'http'):
        variation[name] = {}
        for metric in ('ops_s', 'p99_us'):
            values = [run['reports'][name][metric] for run in layers]
            if not values or any(not math.isfinite(x) or x <= 0 for x in values):
                raise ValueError('Invalid layer metric')
            cv = statistics.stdev(values) / statistics.mean(values) if len(values) > 1 else None
            variation[name][metric + '_cv'] = cv
            if cv is None or cv > .10:
                failures.append(f'Layer repeatability gate failed: {name}/{metric}')
    ready = not failures
    return {'schema': 1, 'parent_acceptance': {'RES-004': ready, 'RES-005': ready},
            'failures': failures, 'layer_variation': variation,
            'publication_eligible': False, 'hypotheses_accepted': [],
            'note': 'These gates validate research infrastructure/controls only, not engine superiority. Review evidence before ticking the roadmap.'}


def analyze(directory):
    directory = Path(directory).resolve()
    verify_seal(directory)
    plan = json.loads((directory / 'plan.json').read_text())
    aa = {str(clients): analyze_ab(directory / ('aa-c' + str(clients))) for clients in (1, 10)}
    layers = []
    for i in range(plan['layer_repetitions']):
        path = directory / f'layers-{i:02d}'
        verify_seal(path, ('manifest.json', 'fixture.json', 'report.json'))
        report = json.loads((path / 'report.json').read_text())
        for row in report['reports'].values():
            if row.get('correct') is not True or row.get('errors') != 0 or row.get('operations') != plan['layer_operations']:
                raise ValueError('Layer correctness/count gate failed')
        layers.append(report)
    return acceptance(aa, layers, plan)


def run(plan, directory):
    directory = Path(directory).resolve()
    if directory.is_relative_to(ROOT) and 'build' not in directory.relative_to(ROOT).parts[:1]:
        raise ValueError('Use a fresh build/ output or a directory outside the repository')
    if shutil.disk_usage(directory.parent if directory.parent.exists() else ROOT).free < 5 * 1024**3:
        raise ValueError('At least 5GiB Linux filesystem free space required; also check Windows VHDX host drive')
    description = json.loads(subprocess.check_output([plan['cpp_path'], '--describe'], text=True, timeout=5))
    if description.get('fixture_version') != 2 or description.get('build_type') != 'Release':
        raise ValueError('Rebuild the Release C++ probe with warm-up support')
    subprocess.run([sys.executable, '-c', 'import uvicorn,httpx,fastapi,pymongo,lark'], check=True, timeout=10)
    initial_source = source_identity()
    directory.mkdir(parents=True, exist_ok=False)
    exclusive_json(directory / 'plan.json', plan)
    exclusive_json(directory / 'source_identity.json', initial_source)
    exclusive_json(directory / 'artifacts.json', {key: digest(plan[key]) for key in ('native_path', 'cpp_path')})
    for cfg in plan['aa_configs']:
        clients = cfg['workload']['clients']
        print(f'A/A starting: clients={clients}', file=sys.stderr, flush=True)
        report = run_ab(cfg, directory / ('aa-c' + str(clients)))
        exclusive_json(directory / f'aa-c{clients}-report.json', report)
    previous = os.sched_getaffinity(0)
    try:
        os.sched_setaffinity(0, plan['cpus'])
        for index in range(plan['layer_repetitions']):
            print(f'Layers repetition {index + 1}/{plan["layer_repetitions"]}', file=sys.stderr, flush=True)
            run_layers(directory / f'layers-{index:02d}', records=plan['layer_records'], operations=plan['layer_operations'],
                       warmup=plan['layer_warmup_operations'], timeout=plan['layer_timeout_seconds'])
    finally:
        os.sched_setaffinity(0, previous)
    expected = json.loads((directory / 'artifacts.json').read_text())
    if any(digest(plan[key]) != sha for key, sha in expected.items()):
        raise ValueError('Artifacts changed during acceptance run')
    if source_identity()['source_sha256'] != initial_source['source_sha256']:
        raise ValueError('Source changed across acceptance phases')
    exclusive_json(directory / 'completed.json', {'sha256': {
        str(path.relative_to(directory)): digest(path) for path in directory.rglob('*')
        if path.is_file() and not any(part.endswith('-db') for part in path.parts)}})
    report = analyze(directory)
    exclusive_json(directory / 'acceptance.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['plan', 'run', 'analyze'])
    parser.add_argument('--profile', choices=['short', 'long'], default='long')
    parser.add_argument('--cpus', help='Comma-separated allowed logical CPU IDs; default first four allowed')
    parser.add_argument('--directory', type=Path)
    args = parser.parse_args()
    if args.action != 'plan' and args.directory is None:
        parser.error('--directory required')
    if args.action == 'analyze':
        result = analyze(args.directory)
    else:
        plan = make_plan(args.profile, [int(value) for value in args.cpus.split(',')] if args.cpus else None)
        result = plan if args.action == 'plan' else run(plan, args.directory)
    print(json.dumps(result, allow_nan=False, indent=2))
    if args.action != 'plan' and not all(result['parent_acceptance'].values()):
        raise SystemExit(2)


if __name__ == '__main__':
    main()
