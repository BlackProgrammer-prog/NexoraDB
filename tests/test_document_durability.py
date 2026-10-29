import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))
sys.path.insert(0, str(ROOT / 'nexora-scale-lab'))
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import create_doc_engine, load_native_module


@pytest.mark.parametrize('profile', ['durable','buffered'])
@pytest.mark.parametrize('auto', [True,False])
def test_service_profile_is_explicit_effective_and_not_changed_by_hardware(tmp_path, profile, auto):
    engine = create_doc_engine(AdminApiSettings(db_path=tmp_path/'db', rocks_memory_mb=16,
                                               rocks_durability=profile, rocks_auto_tune=auto))
    try:
        report = json.loads(engine.durability_report())
        assert report['profile'] == profile
        assert report['sync_on_ack'] == (profile == 'durable')
        assert report['wal_enabled'] and report['manual_wal_flush'] is False
        assert report['scope'].startswith('document engine only')
        assert engine.create_collection('items').success
        assert engine.insert_one('items','{"_id":"one"}').success
        metrics = json.loads(engine.operation_metrics())['write_path']
        assert metrics['durable_ack'] == report['sync_on_ack']
        if profile == 'durable':
            assert metrics['wal_syncs'] > 0
    finally:
        assert engine.prepare_shutdown().success


def test_native_default_is_durable(tmp_path):
    native = load_native_module(AdminApiSettings())
    assert native.RocksSettings().durability == native.DocumentDurability.Durable
    engine = native.DocEngine(str(tmp_path/'db'))
    assert json.loads(engine.durability_report())['sync_on_ack'] is True
    assert engine.prepare_shutdown().success


@pytest.mark.parametrize('value', ['async','true','',[],None])
def test_invalid_profile_cannot_silently_fall_back(value):
    with pytest.raises(RuntimeError, match='durable or buffered'):
        AdminApiSettings(rocks_durability=value).validate_for_startup()


def test_environment_profile_is_normalized_and_validated(monkeypatch):
    monkeypatch.setenv('NEXORADB_ROCKS_DURABILITY','BUFFERED')
    assert AdminApiSettings().rocks_durability == 'buffered'
    monkeypatch.setenv('NEXORADB_ROCKS_DURABILITY','off')
    with pytest.raises(RuntimeError, match='durable or buffered'):
        AdminApiSettings().validate_for_startup()


def test_lab_checks_effective_profile_and_records_it(tmp_path):
    from scale_lab.adapters import NexoraAdapter
    adapter = NexoraAdapter(str(tmp_path/'db'),'scale_lab_items',memory_budget_bytes=16*1024**2)
    adapter.connect()
    try:
        adapter.prepare_schema()
        effective = adapter.metadata()['effective_durability']
        assert effective['profile'] == 'durable'
        assert effective['sync_on_ack'] and effective['wal_enabled']
    finally:
        adapter.disconnect()
