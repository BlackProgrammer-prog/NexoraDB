#!/usr/bin/env python3
"""Freeze research questions and a validated baseline without running a workload."""
import argparse
import json
from pathlib import Path
import shutil

from research_ab import ROOT, digest, exclusive_json, analyze


def freeze(baseline, directory):
    baseline, directory = Path(baseline).resolve(), Path(directory).resolve()
    # Prove the baseline evidence is complete/unmodified before referencing it.
    report = analyze(baseline)
    manifest = json.loads((baseline / 'manifest.json').read_text())
    if manifest['config'].get('require_loaded_artifacts') is not True:
        raise ValueError('Baseline needs actual loaded-artifact verification')
    documents = [ROOT / 'docs/research/PHASE0_WORKLOADS_V1_FA.md',
                 ROOT / 'docs/research/COMPARISON_MATRIX_V1_FA.md',
                 ROOT / 'docs/research/HYPOTHESES_V1_FA.md',
                 ROOT / 'nexora-scale-lab/config/research-products-v1.json']
    directory.mkdir(parents=True, exist_ok=False)
    frozen = {}
    for path in documents:
        # Unique new directory; preserve the exact design text, not only hashes.
        shutil.copyfile(path, directory / path.name)
        frozen[path.name] = digest(path)
    value = {'schema': 1, 'design_documents_sha256': frozen,
             'baseline_manifest_sha256': digest(baseline / 'manifest.json'),
             'baseline_completion_sha256': digest(baseline / 'completed.json'),
             'baseline_binaries': manifest['binaries']['A'],
             'baseline_binary_archive': manifest['binary_archive']['A'],
             'baseline_source_archive_sha256': manifest['source_archive_sha256'],
             'baseline_location': str(baseline), 'baseline_control_classification': report['classification'],
             'baseline_measurements_stable': report['classification'] == 'descriptive-stable',
             'hypotheses': ['H1', 'H2', 'H3', 'H4', 'H5', 'H6'],
             'status': 'preregistered design; no hypothesis accepted',
             'candidate_B': 'must be selected and frozen before a genuine A/B run',
             'peer_runtime_admission': 'required; selected product versions are not installed/verified server claims',
             'publication_eligible': False}
    exclusive_json(directory / 'registration.json', value)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(freeze(args.baseline, args.directory), indent=2))


if __name__ == '__main__':
    main()
