import json
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'NexoraDB/src'))
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import load_native_module


def test_memory_report_and_shutdown_release_page_snapshots(tmp_path):
    native = load_native_module(AdminApiSettings(native_module_path=Path(__file__).resolve().parents[1] / 'nexoradb.cpython-310-x86_64-linux-gnu.so'))
    engine = native.DocEngine(str(tmp_path / 'memory'))
    assert engine.create_collection('items').success
    assert engine.insert_many('items', [json.dumps({'_id': str(n), 'v': n}) for n in range(10)]).success
    report = json.loads(engine.memory_report())
    assert report['cache_usage_bytes'] >= report['cache_pinned_bytes']
    assert report['page_sessions'] == 0
    assert report['page_session_capacity'] == 128
    assert not engine.find_page('items', limit=1001).success
    page = engine.find_page('items', limit=1)
    assert page.success
    assert json.loads(engine.memory_report())['page_sessions'] == 1
    assert engine.prepare_shutdown().success
    after = json.loads(engine.memory_report())
    assert after['page_sessions'] == 0
    assert not engine.is_healthy()
