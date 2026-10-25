"""Read only a bounded suffix of a resource log, including after abrupt WSL exit."""
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
with path.open('rb') as source:
    source.seek(max(0, path.stat().st_size - 512 * 1024))
    lines = source.read().replace(b'\0', b'').splitlines()
for line in reversed(lines):
    try:
        value = json.loads(line)
        groups = value['groups']
    except (ValueError, KeyError):
        continue
    print(json.dumps({'file': str(path), 'monotonic_seconds': value['monotonic_seconds'],
                      'memory': {name: group['memory_sum'] for name, group in groups.items()},
                      'processes': {name: [{'pid': p['pid'], 'rss_bytes': p['rss_bytes'],
                                             'pss_bytes': p.get('pss_bytes')}
                                            for p in group['processes']] for name, group in groups.items()}}, indent=2))
    break
else:
    raise SystemExit('No complete resource snapshot found in suffix')
