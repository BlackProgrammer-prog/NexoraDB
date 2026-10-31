import gc
import os
import json
from pathlib import Path
import sys
import threading
import time
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))


@pytest.fixture
def native():
    from nexoradb_admin.native import load_native_module
    from nexoradb_admin.config import AdminApiSettings
    path = Path(os.environ.get('NEXORADB_TEST_NATIVE_PATH', ROOT / 'build/debug/nexoradb.cpython-310-x86_64-linux-gnu.so'))
    return load_native_module(AdminApiSettings(native_module_path=path))


def report(pool): return json.loads(pool.report())


def test_pool_wait_release_reject_and_component_accounting(native):
    pool = native.ResourceBudget(1024)
    held = pool.reserve('buffer', 1024)
    with pytest.raises(MemoryError): pool.reserve('query', 1)
    received = []
    thread = threading.Thread(target=lambda: received.append(pool.reserve('query', 128, wait_ms=1000)))
    thread.start()
    deadline = time.monotonic() + 2
    while report(pool)['waits_total'] == 0 and time.monotonic() < deadline: time.sleep(.001)
    assert report(pool)['waits_total'] == 1
    held.close(); held.close()
    thread.join(timeout=2); assert not thread.is_alive()
    assert report(pool)['components']['query'] == 128
    received.pop().close()
    assert report(pool)['reserved_bytes'] == 0
    assert report(pool)['peak_reserved_bytes'] <= 1024
    with pool.reserve('workspace', 1024):
        with pytest.raises(MemoryError): pool.reserve('snapshot', 1, wait_ms=5)
    assert report(pool)['reserved_bytes'] == 0


def test_native_query_input_count_and_result_lifetime_share_admission(tmp_path, native):
    pool = native.ResourceBudget(1024**2)
    rocks = native.RocksSettings(); rocks.execution_budget = pool
    engine = native.DocEngine(str(tmp_path / 'db'), rocks_settings=rocks)
    engine.pause_document_migration()
    assert engine.create_collection('items').success
    for start in range(0, 500, 50):
        result = engine.insert_many('items', [json.dumps({'_id': str(i), 'v': 1, 'payload': 'x'*100}) for i in range(start, start+50)])
        assert result.success, result.error_msg
    with pool.reserve('workspace', 1024**2-20000):
        rejected = engine.insert_one('items', json.dumps({'_id': 'rejected', 'payload': 'x'*10000}))
        assert not rejected.success and rejected.error_code == 'resource_exhausted'
        counted = engine.count('items', native.Condition.leaf('v', native.Op.EQ, '1', native.ValueType.Int64))
        assert counted.success and counted.data == '500'
        result = engine.find_many('items')
        assert not result.success and result.error_code == 'resource_exhausted' and not result.data
        assert engine.is_healthy()
    assert not engine.find_by_id('items', 'rejected').success
    result = engine.find_many('items')
    assert result.success and report(pool)['components']['query'] > 0
    del result; gc.collect()
    assert report(pool)['reserved_bytes'] == 0
    assert engine.prepare_shutdown().success


def test_graph_snapshot_and_ingest_buffers_compete_with_query_pool(tmp_path, native):
    pool = native.ResourceBudget(32*1024**2)
    settings = native.RocksSettings(); settings.execution_budget = pool
    engine = native.DocEngine(str(tmp_path/'db'), rocks_settings=settings)
    assert engine.create_collection('nodes').success
    assert engine.insert_many('nodes', ['{"_id":"a"}', '{"_id":"b"}']).success
    manager = native.GraphManager(engine, str(tmp_path/'graphs'))
    definition = native.GraphDefinition(); definition.name = 'g'
    node = native.NodeMappingDef(); node.node_type = 'Node'; node.collection = 'nodes'; node.key_path = '_id'
    definition.node_mappings.append(node)
    assert manager.create_graph(definition)
    assert manager.build_graph('g').success
    baseline = report(pool)['reserved_bytes']
    assert report(pool)['components']['graph'] > 0
    with pool.reserve('query', 32*1024**2-baseline):
        with pytest.raises(MemoryError): manager.create_snapshot('g')
    assert report(pool)['reserved_bytes'] == baseline
    snapshot = manager.create_snapshot('g')
    assert report(pool)['components']['snapshot'] == snapshot.owned_allocation_bytes()
    del snapshot; gc.collect()
    assert report(pool)['reserved_bytes'] == baseline
    assert manager.prepare_shutdown(); manager.shutdown(); del manager; gc.collect()
    assert report(pool)['components']['graph'] == 0
    assert engine.prepare_shutdown().success


def test_resource_pool_shared_across_engines_and_pressure_is_not_io_failure(tmp_path, native):
    pool = native.ResourceBudget(65536)
    settings = native.RocksSettings(); settings.execution_budget = pool
    one = native.DocEngine(str(tmp_path/'one'), rocks_settings=settings)
    two = native.DocEngine(str(tmp_path/'two'), rocks_settings=settings)
    assert one.create_collection('items').success and two.create_collection('items').success
    with one.execution_budget().reserve('query', 65536):
        result = two.insert_one('items', '{"_id":"x"}')
        assert not result.success and result.error_code == 'resource_exhausted'
        assert two.is_healthy()
        assert json.loads(one.memory_report())['execution_budget']['pool_id'] == report(two.execution_budget())['pool_id']
    assert two.insert_one('items', '{"_id":"x"}').success
    assert one.prepare_shutdown().success and two.prepare_shutdown().success


def test_service_sizing_applies_effective_rocks_and_all_margins(tmp_path, native):
    from nexoradb_admin.native import create_doc_engine
    from nexoradb_admin.config import AdminApiSettings
    settings = AdminApiSettings(db_path=tmp_path/'sized', rocks_auto_tune=False, rocks_memory_mb=16,
        resource_process_mb=64, resource_allocator_margin_mb=8, resource_python_margin_mb=8,
        resource_page_cache_margin_mb=8, resource_safety_margin_mb=4)
    engine = create_doc_engine(settings)
    assert report(engine.execution_budget())['capacity_bytes'] == 20*1024**2
    assert engine.prepare_shutdown().success
    rejected = tmp_path/'not-opened'
    with pytest.raises(RuntimeError, match='headroom'):
        create_doc_engine(AdminApiSettings(db_path=rejected, rocks_auto_tune=False, rocks_memory_mb=16,
            execution_memory_mb=21, resource_process_mb=64, resource_allocator_margin_mb=8,
            resource_python_margin_mb=8, resource_page_cache_margin_mb=8, resource_safety_margin_mb=4))
    assert not rejected.exists()


def test_native_select_uses_bounded_cursor_and_holds_response_credit(tmp_path, native):
    from nexoradb.parser import Executor, parse
    pool = native.ResourceBudget(64*1024**2)
    settings = native.RocksSettings(); settings.execution_budget = pool
    engine = native.DocEngine(str(tmp_path/'sql'), rocks_settings=settings)
    assert engine.create_collection('items').success
    assert engine.insert_many('items', ['{"_id":"a","v":1}', '{"_id":"b","v":2}']).success
    executor = Executor(engine)
    response = executor.execute_text('SELECT v FROM items WHERE v >= 1 LIMIT 1;')
    assert response[0]['success'] and response[0]['count'] == 1
    assert response[0]['documents'] == [{'_id':'a', 'v':1}]
    assert 0 < report(pool)['components']['query'] <= 16*1024**2
    assert json.loads(engine.memory_report())['page_live_snapshots'] == 0
    response[0]['documents'].close()
    assert report(pool)['reserved_bytes'] == 0
    with pytest.raises(Exception): parse('SELECT * FROM items ORDER BY v;')
    assert engine.prepare_shutdown().success


def test_document_cursor_metadata_and_page_buffer_release_at_distinct_lifetimes(tmp_path, native):
    pool = native.ResourceBudget(256*1024)
    settings = native.RocksSettings(); settings.execution_budget = pool
    engine = native.DocEngine(str(tmp_path/'pages'), rocks_settings=settings)
    assert engine.create_collection('items').success
    assert engine.insert_many('items', ['{"_id":"a"}', '{"_id":"b"}']).success
    page = engine.find_page('items', limit=1, max_page_bytes=4096)
    assert page.success, page.error_msg
    token = page.continuation_token
    assert report(pool)['components']['snapshot'] > 0 and report(pool)['components']['buffer'] > 0
    del page; gc.collect()
    assert report(pool)['components']['buffer'] == 0 and report(pool)['components']['snapshot'] > 0
    engine.close_page(token)
    assert report(pool)['reserved_bytes'] == 0
    with pool.reserve('workspace', 256*1024-4096):
        page = engine.find_page('items', limit=1, max_page_bytes=4096)
        assert not page.success and 'resource_exhausted' in page.error_msg
        assert json.loads(engine.memory_report())['page_live_snapshots'] == 0
    assert engine.prepare_shutdown().success


def test_open_transaction_staging_holds_credits_until_reset_and_rejects_growth(tmp_path, native):
    pool = native.ResourceBudget(65536)
    settings = native.RocksSettings(); settings.execution_budget = pool
    engine = native.DocEngine(str(tmp_path/'tx-quota'), rocks_settings=settings)
    assert engine.create_collection('items').success
    tx = engine.begin_transaction()
    accepted = 0
    for i in range(100):
        result = engine.insert_one_tx(tx, 'items', json.dumps({'_id':str(i), 'payload':'x'*256}))
        if not result.success:
            assert result.error_code == 'resource_exhausted'; break
        accepted += 1
    assert 0 < accepted < 100 and tx.is_valid()
    assert report(pool)['components']['buffer'] > 0
    assert engine.get_collection_size('items') == 0
    assert engine.rollback_transaction(tx).success
    assert report(pool)['reserved_bytes'] == 0
    assert engine.get_collection_size('items') == 0
    assert engine.prepare_shutdown().success


def test_lookup_join_rejects_capacity_pressure_without_partial_result(tmp_path, native):
    pool = native.ResourceBudget(64*1024**2)
    settings = native.RocksSettings(); settings.execution_budget = pool
    engine = native.DocEngine(str(tmp_path/'join-quota'), rocks_settings=settings)
    for collection in ('users','posts'): assert engine.create_collection(collection).success
    assert engine.insert_one('users','{"_id":"u"}').success
    assert engine.insert_one('posts','{"_id":"p","user":"u"}').success
    result = engine.lookup_join('posts','user','users','_id')
    assert not result['success'] and not result['records'] and 'resource_exhausted' in result['error_msg']
    assert report(pool)['reserved_bytes'] == 0 and engine.is_healthy()
    assert engine.prepare_shutdown().success
