"""JSON/CSV/Markdown/interactive HTML reports from verified real trials only."""
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def interval(values):
    n = len(values)
    mean = statistics.mean(values)
    if n < 2:
        return {'n': n, 'mean': mean, 'stddev': None, 'ci95_low': None, 'ci95_high': None}
    # Student t critical values, two-sided 95%. Beyond 30 df use the conservative
    # 30-df value instead of incorrectly treating n=5 as a normal population.
    critical = [0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306,
                2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110,
                2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056,
                2.052, 2.048, 2.045, 2.042][min(n - 1, 30)]
    deviation = statistics.stdev(values)
    margin = critical * deviation / math.sqrt(n)
    return {'n': n, 'mean': mean, 'stddev': deviation,
            'ci95_low': mean - margin, 'ci95_high': mean + margin}


def deployment_resource(result, metric):
    resources = result['resources']
    if resources.get('missing_metrics'):
        return None
    if metric != 'cpu_seconds':
        return resources['deployment_sampled_peaks'][metric]
    source = resources['deltas'] if metric == 'cpu_seconds' else resources['sampled_peaks']
    combined = resources['attribution'].startswith('native')
    return source['server'][metric] + (0 if combined else source['generator'][metric])


def report(directory):
    directory = Path(directory)
    manifest = json.loads((directory / 'manifest.json').read_text())
    config = manifest['config']
    trials, ingestions, engines, failure_ledger = [], [], [], []
    for job in manifest['schedule']:
        attempts = sorted((directory / ('job-' + job['id'])).glob('attempt-*'))
        if not attempts:
            continue
        for attempt in attempts:
            checkpoint_file = attempt / 'checkpoint.json'
            checkpoint_data = json.loads(checkpoint_file.read_text()) if checkpoint_file.exists() else {'status': 'not-started'}
            if checkpoint_data['status'] != 'completed':
                measurements = [json.loads(p.read_text()) for p in attempt.glob('cell-*/measurement.json')]
                failure_ledger.append({'job': job['id'], 'database': job['database'],
                                       'attempt': str(attempt.relative_to(directory)),
                                       'status': checkpoint_data['status'],
                                       'interrupted_suffix_outcomes': 'unknown' if checkpoint_data['status'] == 'measuring' else None,
                                       'counts': {key: sum(m['counts'].get(key, 0) for m in measurements)
                                                  for key in ('attempted', 'successful', 'failed', 'timeouts', 'unexpected_results')},
                                       'unverified_trials': sum(m.get('status') != 'verified-and-restored' for m in measurements)})
        latest = attempts[-1]
        # Failed/incomplete attempts never get mixed with later reruns.
        if not (latest / 'checkpoint.json').exists():
            continue
        checkpoint = json.loads((latest / 'checkpoint.json').read_text())
        if (latest / 'engine.json').exists():
            engines.append(json.loads((latest / 'engine.json').read_text()))
        if (latest / 'ingestion.json').exists():
            ingestion = json.loads((latest / 'ingestion.json').read_text())
            ingestions.append({**ingestion, 'database': job['database'], 'repetition': job['repetition']})
        for path in sorted(latest.glob('cell-*/measurement.json')):
            value = json.loads(path.read_text())
            if value.get('status') == 'verified-and-restored':
                trials.append(value)
    groups = defaultdict(list)
    for value in trials:
        key = tuple(value[k] for k in ('database', 'workload', 'clients', 'working_set', 'index_policy', 'load_mode', 'distribution'))
        groups[key].append(value)
    aggregates = []
    for key, values in sorted(groups.items()):
        metrics = defaultdict(list)
        counts = defaultdict(int)
        for value in values:
            elapsed = value['elapsed_seconds_including_drain']
            for name in ('attempted', 'successful', 'failed', 'reads', 'updates', 'timeouts', 'rejected_load_slots', 'unexpected_results'):
                counts[name] += value['counts'].get(name, 0)
            for name in ('attempted', 'successful', 'failed', 'reads', 'updates'):
                metrics[name + '_per_second'].append(value['counts'].get(name, 0) / elapsed)
            for name, number in value['latency'].items():
                if name != 'quantile_relative_bucket_error' and number is not None:
                    metrics[name].append(number)
            metrics['error_fraction'].append(value['counts'].get('failed', 0) / max(1, value['counts']['attempted']))
            for metric, label in [('cpu_seconds', 'deployment_cpu_seconds'), ('pss_bytes', 'deployment_peak_pss_bytes')]:
                number = deployment_resource(value, metric)
                if number is not None:
                    metrics[label].append(number)
        aggregate = dict(zip(('database', 'workload', 'clients', 'working_set', 'index_policy', 'load_mode', 'distribution'), key))
        aggregate.update(repetitions=len(values), counts=dict(counts),
                         metrics={name: interval(numbers) for name, numbers in metrics.items()})
        aggregates.append(aggregate)
    ingestion_groups = defaultdict(list)
    for value in ingestions:
        ingestion_groups[(value['database'], value['index_policy'])].append(value)
    ingestion_summary = []
    for (engine, policy), values in sorted(ingestion_groups.items()):
        ingestion_summary.append({'database': engine, 'index_policy': policy, 'repetitions': len(values),
                                  'records_per_second': interval([v['records_per_second'] for v in values]),
                                  'index_build_seconds': interval([v['index_build_seconds'] for v in values]),
                                  'storage_bytes': interval([v['storage']['bytes'] for v in values]),
                                  'storage_scope': values[0]['storage']['scope']})
    expected_trials = sum(len(job['cells']) for job in manifest['schedule'])
    configurations = defaultdict(set)
    for engine in engines:
        if engine['deployment'] == 'embedded':
            key = 'nexoradb'
            stable = {name: engine.get(name) for name in ('native_build', 'native_binary_sha256', 'settings')}
        elif 'server_config' in engine:
            key = 'mongodb'
            stable = {name: engine.get(name) for name in ('server_version', 'driver_version', 'settings', 'server_config')}
        else:
            key = 'postgresql'
            stable = {name: engine.get(name) for name in ('driver_version', 'settings')}
        configurations[key].add(json.dumps(stable, sort_keys=True))
    checks = {
        'comparison_not_native_capacity_pipeline': config.get('native_ingestion_mode','transactional')=='transactional'
            and config.get('native_bson_processes',0)==0,
        'all_jobs_completed': manifest['status'] == 'completed' and len(ingestions) == len(manifest['schedule']),
        'same_revision_no_imports': not manifest.get('imported_jobs'),
        'no_external_cleanup_recovery': not manifest.get('cleanup_recoveries'),
        'randomized_engine_order': config.get('database_order', 'randomized') == 'randomized',
        'all_planned_trials_present': len(trials) == expected_trials,
        'exactly_100m_records': config['records'] == 100_000_000,
        'at_least_five_repetitions': config['repetitions'] >= 5,
        'warmup_at_least_60_seconds': config['warmup_seconds'] >= 60,
        'measurement_at_least_300_seconds': config['measurement_seconds'] >= 300,
        'no_operation_count_truncation': config['max_operations'] is None,
        'all_required_engines': set(config['databases']) == {'nexoradb', 'postgresql', 'mongodb'},
        'both_index_policies': set(config['index_policies']) == {'before-load', 'after-load'},
        'secondary_index_plans_verified': bool(engines) and all(e.get('username_lookup_plan', {}).get('uses_secondary_index', False) for e in engines),
        'stable_versions_binaries_and_settings': bool(configurations) and all(len(values) == 1 for values in configurations.values()),
        'zero_failed_operations': bool(trials) and all(not t['counts'].get('failed', 0) for t in trials),
        'no_failed_or_unverified_attempts': not failure_ledger,
        'full_integrity_and_restoration': config.get('validation_mode', 'full') == 'full' and bool(trials) and all(t['post_validation']['verified_records'] == config['records']
                                     and t['restored_validation']['verified_records'] == config['records'] for t in trials),
        'complete_resource_samples': bool(trials) and all(not t['resources']['missing_metrics'] for t in trials)
                                    and all(not t['resources']['missing_metrics'] for t in ingestions),
        'native_release_build': all(e.get('native_build', {}).get('build_type') == 'Release'
                                    and not any(e['native_build'].get(name) for name in ('asan_enabled', 'ubsan_enabled', 'tsan_enabled'))
                                    for e in engines if e['deployment'] == 'embedded'),
    }
    artifact = {'schema_version': 1, 'comparison_scope': manifest['comparison_scope'],
                'protocol_checks': checks, 'protocol_passed': all(checks.values()),
                'publication_status': ('exploratory sampled-validation profile; does not meet the full publication protocol'
                                       if config.get('validation_mode', 'full') != 'full'
                                       else 'human review required; no automatic engine-only winner claim'),
                'expected_trials': expected_trials, 'completed_trials': len(trials),
                'failed_or_incomplete_attempts': failure_ledger,
                'failure_accounting': 'Raw measurement statistics include failed attempts. Failed/unverified trials are retained in the audit ledger, excluded from primary estimates, and prevent protocol passage even after resume.',
                'aggregates': aggregates, 'ingestion': ingestion_summary,
                'imported_job_provenance': manifest.get('imported_jobs', []),
                'claims': manifest['claims'], 'unsupported': manifest['unsupported'],
                'statistics': 'Mean and sample SD across independent repetition jobs; two-sided Student t 95% intervals. Latency summaries are means of per-trial quantiles, not pooled quantiles.',
                'scope_notes': ['PSS/CPU represent total deployment: server+generator; native is already combined.',
                                'Storage scopes differ across engines; do not infer a storage winner from these footprints.',
                                'Read-only warm-up conditions caches before every workload, including A/B.',
                                'Disjoint worker key ownership excludes shared-key contention.',
                                'Open-loop response latency includes schedule lag and rejected slots; closed-loop has no coordinated-omission correction.',
                                'Confidence intervals describe repeat variability, not proof of causal superiority.']}
    from .protocol import atomic_json
    atomic_json(directory / 'summary.json', artifact)
    with (directory / 'summary.csv').open('w', newline='', encoding='utf-8') as output:
        fields = ['database', 'workload', 'clients', 'working_set', 'index_policy', 'load_mode', 'distribution',
                  'repetitions', 'metric', 'n', 'mean', 'stddev', 'ci95_low', 'ci95_high']
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for value in aggregates:
            for metric, numbers in value['metrics'].items():
                writer.writerow({**{k: value[k] for k in fields[:8]}, 'metric': metric, **numbers})
    with (directory / 'ingestion.csv').open('w', newline='', encoding='utf-8') as output:
        writer = csv.writer(output)
        writer.writerow(['database', 'index_policy', 'repetitions', 'metric', 'mean', 'stddev', 'ci95_low', 'ci95_high', 'scope'])
        for value in ingestion_summary:
            for metric in ('records_per_second', 'index_build_seconds', 'storage_bytes'):
                stats = value[metric]
                writer.writerow([value['database'], value['index_policy'], value['repetitions'], metric,
                                 stats['mean'], stats['stddev'], stats['ci95_low'], stats['ci95_high'], value['storage_scope']])
    lines = ['# Nexora Scale Lab comparison', '', f'Protocol checks passed: {artifact["protocol_passed"]}. Human methodology review remains required.',
             f'Completed trials: {len(trials)} / {expected_trials}. Scope: local deployments.', '', artifact['statistics'], '',
             '| Database | Workload | Clients | Working set | Index policy | Repetitions | Successful ops/s mean | P99 ms mean |',
             '| --- | --- | ---: | ---: | --- | ---: | ---: | ---: |']
    for value in aggregates:
        lines.append(f'| {value["database"]} | {value["workload"]} | {value["clients"]} | {value["working_set"]} | {value["index_policy"]} | {value["repetitions"]} | {value["metrics"]["successful_per_second"]["mean"]:.3f} | {value["metrics"]["p99_ms"]["mean"]:.3f} |')
    lines.extend(['', '## Protocol checks', ''] + [f'- {key}: {value}' for key, value in checks.items()])
    lines.extend(['', '## Failed or incomplete attempts', '', artifact['failure_accounting'], '',
                  '```json', json.dumps(failure_ledger, indent=2), '```'])
    lines.extend(['', '## Scope and limitations', ''] + ['- ' + note for note in artifact['scope_notes'] + artifact['unsupported']])
    (directory / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    template = (Path(__file__).parent / 'report.html').read_text()
    # JSON is embedded as non-executable text; escape HTML delimiters to prevent
    # even a future database/workload label from terminating the script element.
    payload = json.dumps(artifact).replace('&', '\\u0026').replace('<', '\\u003c').replace('>', '\\u003e')
    (directory / 'report.html').write_text(template.replace('__REPORT_DATA__', payload), encoding='utf-8')
    return {'completed_trials': len(trials), 'protocol_passed': artifact['protocol_passed'],
            'report': str(directory / 'report.html')}
