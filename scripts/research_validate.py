#!/usr/bin/env python3
"""Persist bounded phase-0 evidence; no production services or large datasets."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from research_ab import digest, exclusive_json, run as run_ab, source_identity
from research_layers import run as run_layers

ROOT = Path(__file__).resolve().parents[1]


def run(directory, with_cgroup=False):
    directory = Path(directory).resolve()
    if with_cgroup and os.geteuid() != 0:
        raise ValueError('--with-cgroup needs root; parent/controller is never moved')
    directory.mkdir(parents=True, exist_ok=False)
    exclusive_json(directory / 'validation-manifest.json', {'schema': 1, 'source': source_identity(),
                   'records_per_scenario': 1000, 'operations_per_scenario': 200,
                   'clients': [1, 10], 'repetitions': 2, 'publication_eligible': False})
    results = []
    for repetition in range(2):
        for clients in (1, 10):
            for scenario in ('document-oltp', 'document-graph', 'low-memory'):
                argv = [sys.executable, str(ROOT / 'scripts/research_workload.py'), '--scenario', scenario,
                        '--clients', str(clients), '--records', '1000', '--operations', '200']
                if with_cgroup and scenario == 'low-memory':
                    argv = [sys.executable, str(ROOT / 'scripts/research_cgroup.py'), '--', *argv]
                stem = f'r{repetition}-c{clients}-{scenario}'
                with (directory / (stem + '.stdout')).open('x') as stdout, (directory / (stem + '.stderr')).open('x') as stderr:
                    subprocess.run(argv, stdout=stdout, stderr=stderr, check=True, timeout=75)
                row = json.loads((directory / (stem + '.stdout')).read_text())
                if row.get('correct') is not True or row['errors'] != 0:
                    raise ValueError('Scenario failed: ' + stem)
                results.append({'scenario': scenario, 'clients': clients, 'repetition': repetition,
                                'correct': row['correct'], 'memory_limit_enforced': row['memory_limit_enforced']})
                print('Validated ' + stem, file=sys.stderr, flush=True)
    cfg = json.loads((ROOT / 'nexora-scale-lab/config/research-smoke-v1.json').read_text())
    cfg['commands'] = {arm: [sys.executable, str(ROOT / 'scripts/research_workload.py')] for arm in ('A', 'B')}
    cfg['repetitions'] = 5
    cfg['workload'].update(records=1000, operations=200)
    exclusive_json(directory / 'ab-config.json', cfg)
    ab_report = run_ab(cfg, directory / 'aa-control')
    exclusive_json(directory / 'aa-report.json', ab_report)
    layer_report = run_layers(directory / 'layers', records=1000, operations=200)
    result = {'schema': 1, 'scenario_runs': results, 'aa_control': ab_report,
              'layers': {name: {'correct': row['correct'], 'operations': row['operations']}
                         for name, row in layer_report['reports'].items()},
              'publication_eligible': False, 'hypotheses_accepted': []}
    exclusive_json(directory / 'validation-report.json', result)
    exclusive_json(directory / 'completed.json', {'sha256': {str(path.relative_to(directory)): digest(path)
                   for path in sorted(directory.rglob('*')) if path.is_file() and '-db' not in '/'.join(path.parts)}})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--with-cgroup', action='store_true')
    args = parser.parse_args()
    print(json.dumps(run(args.directory, args.with_cgroup), allow_nan=False, indent=2))


if __name__ == '__main__':
    main()
