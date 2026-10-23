import json
from pathlib import Path
import sys

import pytest
from bson import BSON

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'NexoraDB/src'))
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import load_native_module


@pytest.fixture
def engine(tmp_path):
    path=ROOT/'nexoradb.cpython-310-x86_64-linux-gnu.so'
    if not path.exists(): pytest.skip('Linux Release binary required')
    native=load_native_module(AdminApiSettings(native_module_path=path))
    settings=native.RocksSettings();settings.durability=native.DocumentDurability.Durable
    db=native.DocEngine(str(tmp_path/'db'),rocks_settings=settings)
    assert db.create_collection('items').success
    yield db,native
    assert db.prepare_shutdown().success


def test_bulk_atomic_validation_indexes_and_count(engine):
    db,native=engine
    index=native.IndexDefinition();index.index_name='name_idx';index.fields=['name'];index.type=native.IndexType.SingleField
    assert db.create_index('items',index).success
    rows=[{'_id':str(i),'name':'user_'+str(i),'nested':{'text':'فارسی'}} for i in range(103)]
    result=db.insert_many_bson_ingest('items',[BSON.encode(row) for row in rows])
    assert result.success and result.data=='103'
    assert db.get_collection_size('items')==103
    for row in rows: assert json.loads(db.find_by_id('items',row['_id']).data)==row
    condition=native.Condition.leaf('name',native.Op.EQ,'user_10')
    assert json.loads(db.find_many('items',condition).data)==[rows[10]]
    for bad in ([BSON.encode({'_id':'new'}),BSON.encode(rows[0])],
                [BSON.encode({'_id':v}) for v in ('x','z','x')],
                [BSON.encode({'_id':'new'}),b'broken']):
        assert not db.insert_many_bson_ingest('items',bad).success
        assert db.get_collection_size('items')==103
        assert not db.find_by_id('items','new').success
        assert not db.find_by_id('items','x').success
    path=json.loads(db.operation_metrics())['write_path']
    assert path['durable_ack'] and path['wal_enabled'] and not path['unordered_write']
    assert path['wal_sync_latency']['count']>0


def test_unique_index_falls_back_without_bypassing_constraints(engine):
    db,native=engine
    index=native.IndexDefinition();index.index_name='unique_name';index.fields=['name'];index.type=native.IndexType.Unique
    assert db.create_index('items',index).success
    encoded=[BSON.encode({'_id':str(i),'name':'same'}) for i in range(2)]
    assert not db.insert_many_bson_ingest('items',encoded).success
    assert db.get_collection_size('items')==0
    result=db.insert_many_bson_ingest('items',encoded[:1])
    assert result.success and result.data=='1'
    assert not db.insert_many_bson_ingest('items',encoded[1:]).success
    assert db.get_collection_size('items')==1


def test_missing_id_uses_checked_default_generation(engine):
    db,_=engine
    result=db.insert_many_bson_ingest('items',[BSON.encode({'value':1}),BSON.encode({'value':2})])
    assert result.success and result.data=='2'
    assert db.get_collection_size('items')==2


def test_primary_range_fence_backfills_neighbors_and_duplicate_max(engine):
    db,_=engine
    assert db.create_collection('aaa_neighbor').success
    assert db.create_collection('zzz_neighbor').success
    assert db.insert_one('aaa_neighbor','{"_id":"same"}').success
    assert db.insert_one('zzz_neighbor','{"_id":"same"}').success
    def insert(ids):
        return db.insert_many_bson_ingest('items',[BSON.encode({'_id':value}) for value in ids])
    assert insert(['b','z']).success
    assert insert(['m','a','c']).success  # Unsorted backfill must use exact checks.
    before=db.get_collection_size('items')
    assert not insert(['zzzz','z']).success  # Existing max cannot bypass checks.
    assert not db.find_by_id('items','zzzz').success
    assert db.get_collection_size('items')==before
    assert insert(['zzz','zz']).success  # Unsorted but entirely above current max.
    assert insert(['same']).success
    assert db.get_collection_size('aaa_neighbor')==1
    assert db.get_collection_size('zzz_neighbor')==1


@pytest.mark.parametrize('identity',['',42,'x'*1025])
def test_identity_captured_during_validation_still_rejects_invalid_ids(engine,identity):
    db,_=engine
    assert not db.insert_many_bson_ingest('items',[BSON.encode({'_id':identity})]).success
    assert db.get_collection_size('items')==0


def test_nested_identity_is_not_mistaken_for_root_id(engine):
    db,_=engine
    result=db.insert_many_bson_ingest('items',[BSON.encode({'nested':{'_id':'inner'}})])
    assert result.success and result.data=='1'
    row=json.loads(db.find_many('items').data)[0]
    assert row['_id']!='inner' and row['nested']['_id']=='inner'
