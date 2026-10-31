"""Summarize MEM-002 evidence without classifying RSS growth as a leak."""
import argparse
import json
from pathlib import Path
import statistics
import hashlib


def summarize(directory):
    manifest = json.loads((directory / 'report.json').read_text())
    if manifest['status'] != 'passed':
        raise ValueError('campaign did not pass lifecycle checks')
    report = {'schema_version': 1, 'source': str(directory / 'report.json'), 'groups': {},
              'sample_count': 0, 'rss_partition_max_abs_delta_bytes': 0,
              'snapshot_pmr_after_destroy_max_bytes': 0, 'representative_stages': []}
    root = Path(__file__).resolve().parents[1]
    report['hash_matches_artifacts'] = {key: hashlib.sha256(path.read_bytes()).hexdigest() == manifest[key]
        for key, path in [('native_sha256', Path(manifest['native'])),
                          ('runner_sha256', directory / 'runner.py' if (directory / 'runner.py').exists() else root / 'scripts/verify_memory_lifecycle.py'),
                          ('telemetry_sha256', directory / 'telemetry.py' if (directory / 'telemetry.py').exists() else root / 'NexoraDB/src/nexoradb/memory.py')]}
    summaries = []
    for run in manifest['runs']:
        path = Path(run['summary']); summary = json.loads(path.read_text()); summaries.append(summary)
        with (path.parent / 'samples.jsonl').open() as source:
            for line in source:
                sample = json.loads(line); report['sample_count'] += 1
                delta = sample['process']['rss_partition_delta_bytes']
                if delta is None:
                    raise ValueError('RSS/PSS partition unavailable in evidence')
                report['rss_partition_max_abs_delta_bytes'] = max(report['rss_partition_max_abs_delta_bytes'], abs(delta))
                if sample['stage'] == 'destroyed_before_trim':
                    pmr = next(item['value'] for item in sample['measurements'] if item['id'] == 'graph_snapshot_pmr.current_requested_bytes')
                    report['snapshot_pmr_after_destroy_max_bytes'] = max(report['snapshot_pmr_after_destroy_max_bytes'], pmr)
                if run['repetition'] == 0 and run['mode'] == 'control' and sample['cycle'] == manifest['warmup']:
                    report['representative_stages'].append({'stage': sample['stage'],
                        'rss_bytes': sample['process']['rss_bytes'], 'pss_bytes': sample['process']['pss_bytes'],
                        'selected_native_estimate_bytes': sample['reconciliation']['selected_native_estimate_bytes'],
                        'allocator_allocated_arena_bytes': sample['allocator']['allocated_arena_bytes'],
                        'allocator_mmap_bytes': sample['allocator']['mmap_bytes'],
                        'allocator_free_arena_bytes': sample['allocator']['free_arena_bytes'],
                        'python_traced_current_bytes': sample['python']['traced_current_bytes'],
                        'rocks': {key: sample['documents'][0]['raw'].get(key) for key in (
                            'cache_usage_bytes', 'cache_pinned_bytes', 'memtable_usage_bytes',
                            'all_memtables_bytes', 'retained_memtables_bytes', 'page_live_snapshots')}
                            if sample['documents'] else {},
                        'graph_snapshot_pmr_requested_bytes': next(item['value'] for item in sample['measurements']
                            if item['id'] == 'graph_snapshot_pmr.current_requested_bytes')})
    for mode in ('control', 'trim'):
        group = [summary for summary in summaries if summary['mode'] == mode]
        measured = [cycle for summary in group for cycle in summary['cycles'] if not cycle['warmup']]
        report['groups'][mode] = {
            'workers': len(group), 'measured_cycles': len(measured),
            'sampled_rss_peak_bytes': max(summary['sampled_peaks']['rss_bytes'] for summary in group),
            'after_destroy_rss_range_bytes': [min(cycle['after']['rss_bytes'] for cycle in measured), max(cycle['after']['rss_bytes'] for cycle in measured)],
            'after_destroy_pss_range_bytes': [min(cycle['after']['pss_bytes'] for cycle in measured), max(cycle['after']['pss_bytes'] for cycle in measured)],
            'tail_five_rss_range_bytes_per_worker': [[min(cycle['after']['rss_bytes'] for cycle in summary['cycles'][-5:]), max(cycle['after']['rss_bytes'] for cycle in summary['cycles'][-5:])] for summary in group],
            'rss_slope_per_worker_bytes_per_cycle': [summary['statistics']['after']['rss_bytes']['slope_bytes_per_cycle'] for summary in group],
            'allocator_allocated_slope_per_worker_bytes_per_cycle': [summary['statistics']['after']['allocator_allocated_estimate_bytes']['slope_bytes_per_cycle'] for summary in group],
            'python_traced_slope_per_worker_bytes_per_cycle': [summary['statistics']['after']['python_traced_current_bytes']['slope_bytes_per_cycle'] for summary in group],
            'rss_drop_before_after_range_bytes': [min(cycle['before']['rss_bytes'] - cycle['after']['rss_bytes'] for cycle in measured), max(cycle['before']['rss_bytes'] - cycle['after']['rss_bytes'] for cycle in measured)],
            'rss_drop_before_after_median_bytes': statistics.median(cycle['before']['rss_bytes'] - cycle['after']['rss_bytes'] for cycle in measured),
            'trim_seconds_median': statistics.median(cycle['trim']['seconds'] for cycle in measured),
            'trim_seconds_max': max(cycle['trim']['seconds'] for cycle in measured),
            'cycle_seconds_median': statistics.median(cycle['cycle_seconds'] for cycle in measured)}
    report['inference'] = {
        'allocator_retention': 'high confidence when RSS drops within the trim arm after owners are destroyed; released residency, not proof every retained byte is free/reclaimable',
        'live_snapshot_growth': 'PMR ownership checked at each destruction; excludes wrappers and non-PMR allocations',
        'fragmentation': 'possible; mallinfo free arenas and residual RSS cannot prove exact fragmentation',
        'leak': 'not established; observer/tracer/runtime bookkeeping can contribute small live growth even with streamed endpoints; long-run untracked native allocations remain a gap',
        'runtime_policy': 'no automatic trim in telemetry/protocol; preserve diagnostic opt-in, normal allocator/cache lifetimes; no runtime allocator replacement justified',
        'limits': 'bounded stage samples, not continuous peaks/proven ceiling; host not reserved; no 10M/100M or alternative-allocator comparison'}
    (directory / 'analysis.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--directory', type=Path, required=True)
    summarize(parser.parse_args().directory.resolve())
