"""MEM-001/002: ownership, shared-resource deduplication and lifecycle accounting."""
import gc
import importlib.util
import json
import os
from pathlib import Path
import sys
import tracemalloc
import pytest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('memory_telemetry_test', ROOT / 'NexoraDB/src/nexoradb/memory.py')
memory = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(memory)


def test_rollup_reconciles_rss_and_missing_is_not_zero(tmp_path):
    (tmp_path / 'self').mkdir()
    (tmp_path / 'self/smaps_rollup').write_text('Rss: 100 kB\nPss: 60 kB\nPrivate_Clean: 40 kB\nPrivate_Dirty: 40 kB\nShared_Clean: 10 kB\nShared_Dirty: 10 kB\n')
    sample = memory.process_memory(tmp_path)
    assert sample['rss_bytes'] == 102400 and sample['pss_bytes'] == 61440
    assert sample['rss_partition_delta_bytes'] == 0 and sample['pss_file_bytes'] is None
    sample = memory.process_memory(tmp_path / 'missing')
    assert sample['rss_bytes'] is None and sample['pss_bytes'] is None and sample['errors']


class Engine:
    def __init__(self, resource=1, table=30):
        self.raw = dict(schema_version=2, rocks_resource_id=resource, cache_usage_bytes=100, cache_pinned_bytes=80,
            cache_capacity_bytes=1000, memtable_usage_bytes=200, memtable_budget_bytes=1000,
            active_memtables_bytes=120, all_memtables_bytes=180, retained_memtables_bytes=200,
            table_readers_estimate_bytes=table, accounting={})
    def memory_report(self): return json.dumps(self.raw)


class Manager:
    def memory_report(self):
        return json.dumps({'snapshot_pmr_process': {'current_requested_bytes': 10, 'peak_requested_bytes': 20, 'live_resources': 2},
            'scheduler': {'reserved_bytes': 99999},
            'graphs': [{'loaded': True, 'live': {'total_bytes_estimate': 100, 'node_records_bytes': 80, 'budget_bytes': 1000},
                        'cached_snapshot_owned_requested_bytes': 7}]})


def test_deduplicates_shared_cache_and_excludes_aliases_reservations_and_python():
    one, two, manager = Engine(), Engine(), Manager()
    sample = memory.collect_memory([one, two, one], [manager, manager])
    subtotal = sample['reconciliation']['contributions']
    assert subtotal == {'rocks_resource:1.cache_usage_bytes': 100, 'rocks_resource:1.memtable_usage_bytes': 200,
        'document:0.table_readers_estimate_bytes': 30, 'document:1.table_readers_estimate_bytes': 30,
        'graph_manager:0.graph:0.total_bytes_estimate': 100, 'graph_snapshot_pmr.current_requested_bytes': 10}
    assert sample['reconciliation']['selected_native_estimate_bytes'] == 470
    assert sample['reconciliation']['process_total_internal_bytes'] is None
    assert len(sample['documents']) == 2 and len(sample['graph_managers']) == 1
    ids = [item['id'] for item in sample['measurements']]
    assert len(ids) == len(set(ids))


def test_unavailable_estimate_prevents_fake_complete_subtotal_and_observer_does_not_gc_or_trace(monkeypatch):
    tracing = tracemalloc.is_tracing()
    monkeypatch.setattr(gc, 'collect', lambda: pytest.fail('telemetry must not collect GC'))
    sample = memory.collect_memory([Engine(table=None)])
    assert sample['reconciliation']['selected_native_estimate_bytes'] is None
    assert sample['reconciliation']['rss_minus_selected_native_estimates_bytes'] is None
    assert tracemalloc.is_tracing() == tracing
    item = next(item for item in sample['measurements'] if item['id'] == 'document:0.query_buffer_bytes')
    assert item['value'] is None and item['measurement'] == 'unavailable'


@pytest.fixture
def native():
    sys.path.insert(0, str(ROOT / 'NexoraDB/src'))
    from nexoradb_admin.native import load_native_module
    from nexoradb_admin.config import AdminApiSettings
    path = Path(os.getenv('NEXORADB_NATIVE_MODULE_PATH', ROOT / 'nexoradb.cpython-310-x86_64-linux-gnu.so'))
    return load_native_module(AdminApiSettings(native_module_path=path))


def test_schema_contract_disallows_a_fake_process_total():
    schema = json.loads((ROOT / 'docs/telemetry/memory-v1.schema.json').read_text())
    sample = memory.collect_memory([Engine()], [Manager()])
    assert set(schema['required']) <= sample.keys()
    assert schema['properties']['reconciliation']['properties']['process_total_internal_bytes'] == {'type': 'null'}
    item_schema = schema['properties']['measurements']['items']
    for item in sample['measurements']:
        assert set(item_schema['required']) == item.keys()
        assert item['unit'] in item_schema['properties']['unit']['enum']
        assert item['measurement'] in item_schema['properties']['measurement']['enum']
        assert item['value'] is None or item['value'] >= 0
        assert (item['value'] is None) == (item['measurement'] == 'unavailable')


def test_real_shared_resources_and_failed_query_have_correct_ownership(tmp_path, native):
    rocks = native.RocksSettings()
    shared = native.RocksResources(16 * 1024**2, 8 * 1024**2)
    rocks.shared_resources = shared
    one = native.DocEngine(str(tmp_path / 'one'), rocks_settings=rocks)
    two = native.DocEngine(str(tmp_path / 'two'), rocks_settings=rocks)
    one.pause_document_migration(); two.pause_document_migration()
    assert one.create_collection('items').success
    assert one.insert_many('items', ['{"_id":"a"}', '{"_id":"b"}']).success
    assert not one.find_many('missing').success
    sample = memory.collect_memory([one, two], native=native)
    reports = [item['raw'] for item in sample['documents']]
    assert reports[0]['rocks_resource_id'] == reports[1]['rocks_resource_id']
    assert reports[0]['active_query_executions'] == 0
    assert len([key for key in sample['reconciliation']['contributions'] if key.endswith('.cache_usage_bytes')]) == 1
    page = one.find_page('items', limit=1)
    assert page.success
    assert json.loads(one.memory_report())['page_live_snapshots'] == 1
    one.close_page(page.continuation_token)
    assert json.loads(one.memory_report())['page_live_snapshots'] == 0
    assert one.prepare_shutdown().success and two.prepare_shutdown().success


def test_cached_clone_and_external_snapshot_bytes_release_with_actual_owner(tmp_path, native):
    baseline = dict(native.graph_snapshot_memory())
    engine = native.DocEngine(str(tmp_path / 'db'))
    assert engine.create_collection('items').success
    assert engine.insert_many('items', ['{"_id":"a"}', '{"_id":"b"}']).success
    manager = native.GraphManager(engine, str(tmp_path / 'graphs'))
    definition = native.GraphDefinition(); definition.name = 'items'
    node = native.NodeMappingDef(); node.node_type = 'Item'; node.collection = 'items'; node.key_path = '_id'
    definition.node_mappings.append(node)
    assert manager.create_graph(definition) and manager.build_graph('items').success
    options = native.SnapshotOptions(); options.use_cache = True
    first = manager.create_snapshot('items', options=options)
    second = manager.create_snapshot('items', options=options)
    report = json.loads(manager.memory_report())
    cached = report['graphs'][0]['cached_snapshot_owned_requested_bytes']
    assert cached > 0 and first.owned_allocation_bytes() > 0
    assert report['snapshot_pmr_process']['current_requested_bytes'] - baseline['current_requested_bytes'] == cached + first.owned_allocation_bytes() + second.owned_allocation_bytes()
    # clone's previous API includes an admission baseline; it must not be counted as another allocation.
    assert first.allocation_current_bytes() > first.owned_allocation_bytes()
    tiny = native.SnapshotOptions(); tiny.use_cache = True; tiny.max_build_bytes = 1
    with pytest.raises((RuntimeError, ValueError, IndexError)):
        manager.create_snapshot('items', options=tiny)
    assert dict(native.graph_snapshot_memory())['current_requested_bytes'] == report['snapshot_pmr_process']['current_requested_bytes']
    assert manager.prepare_shutdown(); manager.shutdown()
    del manager
    gc.collect()
    assert native.graph_snapshot_memory()['current_requested_bytes'] - baseline['current_requested_bytes'] == first.owned_allocation_bytes() + second.owned_allocation_bytes()
    del first, second
    gc.collect()
    assert native.graph_snapshot_memory()['current_requested_bytes'] == baseline['current_requested_bytes']
    assert native.graph_snapshot_memory()['live_resources'] == baseline['live_resources']
    assert engine.prepare_shutdown().success
