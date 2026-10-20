"""Non-CI RTO fixture; output is JSON, no production SLA is asserted."""
import json
from pathlib import Path
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_admin_recovery import admin, fixture

records = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
with tempfile.TemporaryDirectory(prefix="nexora_rto_") as temporary:
    root = Path(temporary)
    started = time.monotonic()
    fixture(root / "source", records, random_payload=True)
    load_seconds = time.monotonic() - started
    started = time.monotonic()
    report = admin.backup(root / "source/documents", root / "source/graphs", root / "backup")
    backup_seconds = time.monotonic() - started
    backup_bytes = sum(p.stat().st_size for p in admin.regular_files(root / "backup"))
    restores = []
    for repeat in range(3):
        restored = admin.restore(root / "backup", root / f"restored-{repeat}")
        assert restored["report"] == report
        restores.append(restored["elapsed_seconds"])
    assert all(seconds <= 60 for seconds in restores), "local engineering RTO target exceeded"
    print(json.dumps(dict(records=records, documents=report["documents"]["documents"],
                         graph=report["graphs"], backup_bytes=backup_bytes,
                         load_seconds=load_seconds, backup_seconds=backup_seconds,
                         restore_verified_seconds=restores)), flush=True)
