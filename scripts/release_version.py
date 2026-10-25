#!/usr/bin/env python3
"""Synchronize release metadata from nexoradb/_version.py without importing it."""
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CANONICAL = 'NexoraDB/src/nexoradb/_version.py'
VERSION = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'


def plan(root=ROOT, requested=None):
    canonical = (root / CANONICAL).read_text(encoding='utf-8')
    match = re.search(r'^__version__\s*=\s*"([^"]+)"', canonical, re.M)
    if match is None:
        raise ValueError('Missing canonical __version__')
    version = requested if requested is not None else match.group(1)
    if re.fullmatch(VERSION, version) is None:
        raise ValueError('Use a three-component release number, e.g. 0.4.0; prerelease syntax is not supported by this synchronizer')
    updates = {}

    def rewrite(name, transform):
        old = (root / name).read_bytes()
        text = old.decode('utf-8')
        new = transform(text).encode('utf-8')
        if new != old:
            updates[name] = new

    def substitute(text, pattern, replacement, count=1):
        result, matches = re.subn(pattern, replacement, text, count=count, flags=re.M)
        if matches != count:
            raise ValueError('Unexpected version-file structure: '+pattern)
        return result

    rewrite(CANONICAL, lambda s: substitute(s, r'^__version__\s*=\s*"[^"]+"', '__version__ = "'+version+'"'))
    rewrite('CMakeLists.txt', lambda s: substitute(s, r'(project\(NexoraDB\s+VERSION\s+)[0-9]+\.[0-9]+\.[0-9]+', lambda m: m[1]+version))
    rewrite('NexoraDB/pyproject.toml', lambda s: substitute(s, r'^version\s*=\s*"[^"]+"', 'version = "'+version+'"'))
    rewrite('vcpkg.json', lambda s: substitute(s, r'("version"\s*:\s*")[^"]+(\")', lambda m: m[1]+version+m[2]))
    rewrite('frontend/package.json', lambda s: substitute(s, r'("version"\s*:\s*")[^"]+(\")', lambda m: m[1]+version+m[2]))

    def lockfile(text):
        data = json.loads(text)
        data['version'] = version
        data['packages']['']['version'] = version
        output = json.dumps(data, indent=2, ensure_ascii=False)+'\n'
        return output.replace('\n', '\r\n') if '\r\n' in text else output
    rewrite('frontend/package-lock.json', lockfile)
    def readme(text):
        text = substitute(text, r'(pip install NexoraDataBase==)[0-9]+\.[0-9]+\.[0-9]+', lambda m: m[1]+version)
        return substitute(text, r'^Release [0-9]+\.[0-9]+\.[0-9]+: \[changes and upgrade precautions\]\(docs/releases/v[^)]+\.md\)\.',
                          'Release '+version+': [changes and upgrade precautions](docs/releases/v'+version+'.md).')
    rewrite('README.md', readme)
    rewrite('NexoraDB/README.md', lambda s: substitute(s,
        r'^Current version: \*\*[0-9]+\.[0-9]+\.[0-9]+\*\*\. \[Release notes\]\(https://github.com/BlackProgrammer-prog/NexoraDB/blob/main/docs/releases/v[^)]+\.md\)\.',
        'Current version: **'+version+'**. [Release notes](https://github.com/BlackProgrammer-prog/NexoraDB/blob/main/docs/releases/v'+version+'.md).'))
    return version, updates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--set', metavar='VERSION', help='Set canonical version and synchronize all current metadata')
    group.add_argument('--sync', action='store_true', help='Synchronize after editing the canonical file')
    group.add_argument('--check', action='store_true', help='Read-only drift check')
    args = parser.parse_args()
    version, updates = plan(requested=args.set)
    if args.check:
        for name in updates:
            print('Out of sync: '+name, file=sys.stderr)
        print('Canonical version: '+version)
        return int(bool(updates))
    # Validate every input before changing any file. This is mechanical release
    # metadata synchronization; historical reports/release notes are excluded.
    for name, content in updates.items():
        (ROOT / name).write_bytes(content)
        print('Updated '+name)
    print('Release metadata synchronized: '+version)
    print('Rebuild native/dashboard and validate the installed wheel before publishing.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
