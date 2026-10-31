"""Offline copy migration using an actual archived v2 reader in isolated processes."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys

import pytest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'NexoraDB/src'))
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import load_native_module

OLD=Path(os.environ.get('NEXORA_V2_BINARY',str(ROOT/'build/research-phase0-verified-20261028/aa-control/binaries/73039c9c4107e9fab3fac5b5f9a621b00c13c0a1c7aacc79cb94537412acd094/nexoradb.cpython-310-x86_64-linux-gnu.so')))
WORKER=r'''
import importlib.util,json,sys
spec=importlib.util.spec_from_file_location('nexoradb',sys.argv[1]);native=importlib.util.module_from_spec(spec)
sys.modules['nexoradb']=native;spec.loader.exec_module(native)
engine=native.DocEngine(sys.argv[2])
engine.pause_document_migration()
if sys.argv[3] in ('create','create-v2-manifest'):
    assert engine.create_collection('items').success
    index=native.IndexDefinition();index.index_name='v';index.fields=['v'];index.type=native.IndexType.Unique
    assert engine.create_index('items',index).success
    docs=['{"_id":"a","v":1}','{"_id":"b","v":'+sys.argv[4]+'}']
    assert engine.insert_many('items',docs).success
    assert engine.create_internal_app_token('retained-token','{}').success
    if sys.argv[3]=='create-v2-manifest':
        assert engine.migrate_legacy_documents().success
        result=engine.upgrade_format_manifest()
        assert result.success,result.error_msg
        assert json.loads(result.data)['indexes']==2
if sys.argv[3]=='hold':
    print('READY',flush=True);sys.stdin.readline()
if sys.argv[3]=='read':
    assert engine.get_collection_size('items')==2
    assert engine.find_by_id('items','a').success and engine.find_by_id('items','b').success
    assert engine.is_internal_app_token_active('retained-token')
assert engine.prepare_shutdown().success
'''


@pytest.fixture
def native():
    if not OLD.is_file(): pytest.skip('Set NEXORA_V2_BINARY to a genuine archived v2 native reader')
    return load_native_module(AdminApiSettings())


def legacy(path, second='2.5', mode='create'):
    subprocess.run([sys.executable,'-c',WORKER,str(OLD),str(path),mode,second],check=True,timeout=15)


@pytest.mark.parametrize('mode',['create','create-v2-manifest'])
def test_dry_run_copy_reopen_source_retained_and_actual_old_reader_refuses(native,tmp_path,mode):
    source=tmp_path/'source';target=tmp_path/'new';legacy(source,mode=mode)
    with pytest.raises(RuntimeError,match='Legacy|migration'):
        native.DocEngine(str(source))
    audit=native.DocEngine.migrate_index_format_copy(str(source))
    assert audit.success,audit.error_msg
    assert json.loads(audit.data)['dry_run'] and not target.exists()
    assert not native.DocEngine.migrate_index_format_copy(str(source),str(target),True).success
    assert not target.exists()
    result=native.DocEngine.migrate_index_format_copy(str(source),str(target),False)
    assert result.success,result.error_msg
    report=json.loads(result.data)
    assert report['verified'] and report['source_unchanged']
    engine=native.DocEngine(str(target))
    try:
        assert engine.get_collection_size('items')==2
        assert engine.is_internal_app_token_active('retained-token')
        assert engine.integrity_check().success
        assert engine.get_indexes('items')[0].format_version==3
        assert not engine.insert_one('items','{"_id":"alias","v":1.0}').success
    finally:
        assert engine.prepare_shutdown().success
        del engine
    rejected=subprocess.run([sys.executable,'-c',WORKER,str(OLD),str(target),'read'],capture_output=True,text=True,timeout=15)
    assert rejected.returncode!=0 and ('format' in rejected.stderr.lower() or 'version' in rejected.stderr.lower())
    # Rollback is choosing the untouched source with its original reader.
    subprocess.run([sys.executable,'-c',WORKER,str(OLD),str(source),'read'],check=True,timeout=15)
    assert not native.DocEngine.migrate_index_format_copy(str(source),str(target),False).success


def test_numeric_collision_is_reported_without_deleting_source(native,tmp_path):
    source=tmp_path/'source';legacy(source,'1.0')
    audit=native.DocEngine.migrate_index_format_copy(str(source))
    assert not audit.success and 'collision' in audit.error_msg
    target=tmp_path/'conflict'
    copied=native.DocEngine.migrate_index_format_copy(str(source),str(target),False)
    assert not copied.success and 'collision' in copied.error_msg
    with pytest.raises(RuntimeError,match='format'):
        native.DocEngine(str(target))
    subprocess.run([sys.executable,'-c',WORKER,str(OLD),str(source),'read'],check=True,timeout=15)


def test_source_must_be_closed_and_destination_cannot_be_nested(native,tmp_path):
    source=tmp_path/'source';legacy(source)
    child=subprocess.Popen([sys.executable,'-c',WORKER,str(OLD),str(source),'hold'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout,selectors.EVENT_READ)
            assert selector.select(10)
            assert child.stdout.readline().strip()=='READY'
        assert not native.DocEngine.migrate_index_format_copy(str(source)).success
    finally:
        child.communicate('\n',timeout=10)
    result=native.DocEngine.migrate_index_format_copy(str(source),str(source/'nested'),False)
    assert not result.success and not (source/'nested').exists()


def test_sigkill_during_copy_keeps_source_and_fences_partial_target(native,tmp_path):
    tool=ROOT/'build/release/nexora_index_migrate'
    if not tool.exists(): pytest.skip('Build nexora_index_migrate target')
    source=tmp_path/'source';target=tmp_path/'partial';legacy(source)
    with (tmp_path/'stderr.log').open('w+') as log:
        child=subprocess.Popen([str(tool),'--source',str(source),'--destination',str(target),'--apply','--test-pause-after','1'],
                               stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(child.stderr,selectors.EVENT_READ)
                assert selector.select(10)
                line=child.stderr.readline();log.write(line)
                assert 'migration-progress' in line,line
            child.kill();assert child.wait(timeout=5)==-9
        finally:
            if child.poll() is None: child.kill();child.wait(timeout=5)
            child.stdin.close();child.stdout.close();child.stderr.close()
    with pytest.raises(RuntimeError,match='format'):
        native.DocEngine(str(target))
    subprocess.run([sys.executable,'-c',WORKER,str(OLD),str(source),'read'],check=True,timeout=15)
    result=native.DocEngine.migrate_index_format_copy(str(source),str(tmp_path/'retry'),False)
    assert result.success,result.error_msg
