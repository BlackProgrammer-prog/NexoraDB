"""Read-only baseline audit and repeatability summary. Never starts a database."""
import argparse
import hashlib
import json
import statistics
from pathlib import Path


def audit(directory):
    directory = Path(directory)
    manifest = json.loads((directory / 'manifest.json').read_text())
    groups = {}
    artifacts = {}
    for path in sorted(directory.glob('job-*/attempt-*/*.json')):
        artifacts[str(path.relative_to(directory))] = hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted(directory.glob('job-*/attempt-*/cell-*/measurement.json')):
        artifacts[str(path.relative_to(directory))] = hashlib.sha256(path.read_bytes()).hexdigest()
        checkpoint = json.loads((path.parent.parent / 'checkpoint.json').read_text())
        if checkpoint.get('status') != 'completed':
            continue
        job = checkpoint['job']
        value = json.loads(path.read_text())
        key = (job['database'], value['workload'], value['clients'],
               value['working_set'], job['index_policy'])
        groups.setdefault(key, []).append(value['throughput']['successful_per_second'])
    rows = []
    for key, values in sorted(groups.items()):
        mean = statistics.mean(values)
        sd = statistics.stdev(values) if len(values) > 1 else None
        rows.append(dict(zip(('database', 'workload', 'clients', 'working_set', 'index_policy'), key),
                         n=len(values), mean_ops=mean, median_ops=statistics.median(values),
                         sample_sd=sd, cv_percent=100 * sd / mean if sd is not None and mean else None))
    return {'status': manifest['status'], 'source_sha256': manifest['source_sha256'],
            'config_sha256': manifest['config_sha256'], 'config': manifest['config'],
            'artifacts_sha256': artifacts, 'groups': rows,
            'interpretation': 'CV measures observed run-to-run variation, not causality. '
                              'Compare identical configurations and inspect memory pressure; '
                              'native engine and generator share a process and cannot be isolated.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.directory), indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
