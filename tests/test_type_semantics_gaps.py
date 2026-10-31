"""Former COR-004 numeric gaps: all must pass under fenced index format v3."""
from pathlib import Path
import json
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'NexoraDB/src'))
from nexoradb_admin.native import load_native_module
from nexoradb_admin.config import AdminApiSettings


@pytest.mark.parametrize('first,second', [('1','1.0'), ('0.0','-0.0'), ('0','-0.0')])
def test_unique_must_follow_numeric_equality_after_versioned_migration(tmp_path,first,second):
    native=load_native_module(AdminApiSettings())
    engine=native.DocEngine(str(tmp_path/'db'))
    try:
        assert engine.create_collection('items').success
        index=native.IndexDefinition();index.index_name='v_unique';index.fields=['v'];index.type=native.IndexType.Unique
        assert engine.create_index('items',index).success
        assert engine.insert_one('items','{"_id":"first","v":'+first+'}').success
        result=engine.insert_one('items','{"_id":"second","v":'+second+'}')
        assert not result.success, 'Numerically equal unique values must conflict under target semantics'
    finally:
        assert engine.prepare_shutdown().success


def test_numeric_alias_race_and_bulk_rollback(tmp_path):
    native=load_native_module(AdminApiSettings())
    engine=native.DocEngine(str(tmp_path/'db'))
    try:
        assert engine.create_collection('items').success
        index=native.IndexDefinition();index.index_name='v_unique';index.fields=['v'];index.type=native.IndexType.Unique
        assert engine.create_index('items',index).success
        barrier=threading.Barrier(2)
        def insert(index):
            barrier.wait()
            return engine.insert_one('items','{"_id":"'+str(index)+'","v":'+('1' if index==0 else '1.0')+'}').success
        with ThreadPoolExecutor(max_workers=2) as pool:
            assert sum(pool.map(insert,range(2)))==1
        assert engine.get_collection_size('items')==1
        assert not engine.insert_many('items',['{"_id":"new","v":2}','{"_id":"duplicate","v":1.0}']).success
        assert not engine.find_by_id('items','new').success
        assert engine.get_collection_size('items')==1
        assert engine.integrity_check().success
    finally:
        assert engine.prepare_shutdown().success


def test_compound_numeric_aliases_and_exact_large_integer(tmp_path):
    native=load_native_module(AdminApiSettings())
    engine=native.DocEngine(str(tmp_path/'db'))
    try:
        assert engine.create_collection('items').success
        index=native.IndexDefinition();index.index_name='tenant_v';index.fields=['tenant','v'];index.type=native.IndexType.Unique
        assert engine.create_index('items',index).success
        assert engine.insert_one('items','{"_id":"a","tenant":"t","v":9007199254740992}').success
        assert not engine.insert_one('items','{"_id":"alias","tenant":"t","v":9007199254740992.0}').success
        assert engine.insert_one('items','{"_id":"precise","tenant":"t","v":9007199254740993}').success
        assert engine.insert_one('items','{"_id":"other","tenant":"other","v":9007199254740992.0}').success
        assert engine.integrity_check().success
    finally:
        assert engine.prepare_shutdown().success


def test_numeric_update_race_delete_and_reuse(tmp_path):
    native=load_native_module(AdminApiSettings())
    engine=native.DocEngine(str(tmp_path/'db'))
    try:
        assert engine.create_collection('items').success
        index=native.IndexDefinition();index.index_name='v_unique';index.fields=['v'];index.type=native.IndexType.Unique
        assert engine.create_index('items',index).success
        assert engine.insert_many('items',['{"_id":"a","v":2}','{"_id":"b","v":3}']).success
        barrier=threading.Barrier(2)
        def update(number):
            spec=native.UpdateSpec()
            spec.set('v','1' if number==0 else '1.0',
                     native.UpdateValueType.Int64 if number==0 else native.UpdateValueType.Float64)
            barrier.wait()
            return engine.update_by_id('items','a' if number==0 else 'b',spec).success
        with ThreadPoolExecutor(max_workers=2) as pool:
            assert sum(pool.map(update,range(2)))==1
        rows={key:json.loads(engine.find_by_id('items',key).data)['v'] for key in ('a','b')}
        winner=next(key for key,value in rows.items() if value==1)
        assert engine.get_collection_size('items')==2
        assert engine.integrity_check().success
        assert engine.delete_by_id('items',winner).success
        assert engine.insert_one('items','{"_id":"reuse","v":1.0}').success
        assert not engine.insert_one('items','{"_id":"duplicate","v":1}').success
        assert engine.get_collection_size('items')==2
        assert engine.integrity_check().success
    finally:
        assert engine.prepare_shutdown().success
