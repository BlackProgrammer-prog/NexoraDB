import sys
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'NexoraDB/src'))
from nexoradb_admin.hardware import MIB, cgroup_directories, detect, recommend
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import create_doc_engine


def fixture():
    return {
        '/fakeproc/self/cgroup': '0::/tenant/job',
        '/fakeproc/self/mountinfo': '10 1 0:20 /tenant /cg rw - cgroup2 cgroup rw',
        '/fakeproc/meminfo': f'MemTotal: {64 * 1024**2} kB\nMemAvailable: {32 * 1024**2} kB',
        '/cg/cpu.max': '150000 100000', '/cg/job/cpu.max': '400000 100000',
        '/cg/memory.max': str(2 * 1024 * MIB), '/cg/memory.high': 'max',
        '/cg/memory.current': str(1024 * MIB),
        '/cg/job/memory.max': 'max', '/cg/job/memory.high': 'max',
    }


def test_parent_quotas_and_namespace_mount_root(tmp_path):
    files = fixture()
    reader = lambda path: files.get(str(path))
    assert cgroup_directories(Path('/fakeproc'), reader) == [Path('/cg/job'), Path('/cg')]
    observed = detect(tmp_path, proc=Path('/fakeproc'), sys_root=tmp_path, read_text=reader, affinity=list(range(8)))
    assert observed['effective_cpu_threads'] == 1
    assert observed['cpu_quota_cores'] == 1.5
    assert observed['memory_limit_bytes'] == 2048 * MIB
    assert observed['memory_headroom_bytes'] == 1024 * MIB
    result = recommend(observed, 1024)
    assert result['memory_budget_bytes'] == 512 * MIB
    assert result['max_background_jobs'] == 1


def test_unknown_data_is_not_guessed_and_memory_never_expands(tmp_path):
    observed = detect(tmp_path, proc=tmp_path, sys_root=tmp_path, read_text=lambda path: None, affinity=[3, 5])
    assert observed['disk_class'] == 'unknown'
    assert observed['memory_limit_bytes'] is None
    assert recommend(observed, 64)['memory_budget_bytes'] == 64 * MIB
    assert recommend(observed)['cpu_threads'] == 2


def test_fractional_quota_and_zero_headroom(tmp_path):
    files = fixture()
    files['/cg/cpu.max'] = '50000 100000'
    observed = detect(tmp_path, proc=Path('/fakeproc'), sys_root=tmp_path,
                      read_text=lambda path: files.get(str(path)), affinity=[0, 1])
    assert observed['effective_cpu_threads'] == 1
    observed['memory_headroom_bytes'] = 0
    with pytest.raises(RuntimeError, match='Insufficient'):
        recommend(observed)


def test_explicit_cache_conflict_is_not_silently_ignored():
    observed = {'memory_limit_bytes': 128*MIB, 'memory_headroom_bytes': 64*MIB,
                'effective_cpu_threads': 2, 'disk_class': 'ssd'}
    with pytest.raises(RuntimeError, match='Explicit cache'):
        recommend(observed, 256, 128)
    result = recommend(observed, 256, 16)
    assert result['memory_budget_bytes'] == 32*MIB
    assert result['block_cache_bytes'] == 16*MIB


def test_disk_probe_reads_device_not_path_name(tmp_path, monkeypatch):
    import os
    device = tmp_path.stat().st_dev
    node = tmp_path / f'dev/block/{os.major(device)}:{os.minor(device)}/queue'
    node.mkdir(parents=True)
    (node / 'rotational').write_text('1')
    assert detect(tmp_path, proc=tmp_path, sys_root=tmp_path, affinity=[0])['disk_class'] == 'rotational'


def test_admin_applies_policy_and_disabled_mode_never_probes(tmp_path):
    observed = {'memory_limit_bytes': 512*MIB, 'memory_headroom_bytes': 256*MIB,
                'effective_cpu_threads': 1, 'disk_class': 'unknown'}
    rocks = SimpleNamespace()
    engine = Mock()
    engine.is_healthy.return_value = True
    engine.durability_report.return_value = json.dumps({'profile':'durable', 'wal_enabled':True,
        'sync_on_ack':True, 'manual_wal_flush':False})
    native = SimpleNamespace(RocksSettings=lambda: rocks, TransactionSettings=lambda: None,
        DocumentDurability=SimpleNamespace(Durable='durable', Buffered='buffered'),
        DiskClass=SimpleNamespace(SSD='ssd', Rotational='hdd', Unknown='unknown'), DocEngine=Mock(return_value=engine))
    with patch('nexoradb_admin.native.load_native_module', return_value=native), \
         patch('nexoradb_admin.hardware.detect', return_value=observed) as probe:
        create_doc_engine(AdminApiSettings(db_path=tmp_path, rocks_auto_tune=True))
        assert rocks.memory_budget_bytes == 128*MIB
        assert rocks.cpu_threads == rocks.max_background_jobs == 1
        assert rocks.durability == 'durable'  # Hardware policy must preserve explicit service profile.
        probe.reset_mock()
        create_doc_engine(AdminApiSettings(db_path=tmp_path, rocks_auto_tune=False))
        assert rocks.memory_budget_bytes == 256*MIB
        probe.assert_not_called()


def test_invalid_environment_toggle_rejected(monkeypatch):
    monkeypatch.setenv('NEXORADB_ROCKS_AUTO_TUNE', 'typo')
    with pytest.raises(RuntimeError, match='true or false'):
        AdminApiSettings().validate_for_startup()
