"""COR-005: explicit interleavings, no timing-based sleeps or large datasets."""
import json
from pathlib import Path
import sys
from concurrent.futures import ThreadPoolExecutor
import threading
import pytest

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'scripts'),str(ROOT/'NexoraDB/src')]
from transaction_history import check_read_committed, check_strict_serializable
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import load_native_module


@pytest.fixture
def db(tmp_path):
    native=load_native_module(AdminApiSettings())
    settings=native.TransactionSettings();settings.lock_timeout_ms=40;settings.expiration_ms=5000
    engine=native.DocEngine(str(tmp_path/'db'),settings)
    assert engine.create_collection('items').success
    handles=[]
    class Tracked:
        def __getattr__(self,name): return getattr(engine,name)
        def begin_transaction(self):
            tx=engine.begin_transaction();handles.append(tx);return tx
    yield native,Tracked()
    # Pytest retains failed-frame locals; close handles explicitly before the
    # shutdown DDL gate, even when an assertion fails midway through a history.
    for tx in handles:
        if tx.is_valid(): assert engine.rollback_transaction(tx).success
    assert engine.integrity_check().success
    assert engine.prepare_shutdown().success


def value(engine,tx,key,locking=False):
    result=engine.find_by_id_tx(tx,'items',key,for_update=locking)
    assert result.success,result.error_msg
    return json.loads(result.data)['v']


def setv(native,number):
    return native.UpdateSpec().set('v',str(number),native.UpdateValueType.Int64)


def test_read_committed_own_writes_abort_and_nonrepeatable_read(db):
    n,e=db
    assert e.insert_one('items','{"_id":"a","v":0}').success
    a=e.begin_transaction();b=e.begin_transaction()
    assert value(e,a,'a')==0
    assert e.update_by_id_tx(b,'items','a',setv(n,1)).success
    assert value(e,b,'a')==1 and value(e,a,'a')==0  # no dirty read; own writes visible
    assert e.commit_transaction(b).success
    assert value(e,a,'a')==1  # no transaction-wide snapshot
    assert e.update_by_id_tx(a,'items','a',setv(n,9)).success
    assert e.rollback_transaction(a).success
    assert json.loads(e.find_by_id('items','a').data)['v']==1
    history=[{'tx':'a','op':'begin'},{'tx':'b','op':'begin'},
             {'tx':'a','op':'read','key':'a','value':0},{'tx':'b','op':'write','key':'a','value':1},
             {'tx':'b','op':'read','key':'a','value':1},{'tx':'a','op':'read','key':'a','value':0},
             {'tx':'b','op':'commit'},{'tx':'a','op':'read','key':'a','value':1},
             {'tx':'a','op':'write','key':'a','value':9},{'tx':'a','op':'abort'}]
    assert check_read_committed({'a':0},history)['state']=={'a':1}
    assert not check_read_committed({'a':0},history[:5]+[{'tx':'a','op':'read','key':'a','value':1}]+history[5:])['valid']


def test_stale_read_modify_write_is_allowed_not_serializable(db):
    n,e=db
    assert e.insert_one('items','{"_id":"a","v":0}').success
    a=e.begin_transaction();b=e.begin_transaction()
    observed=[value(e,a,'a'),value(e,b,'a')]
    assert observed==[0,0]
    history=[{'tx':t,'op':'begin'} for t in ('a','b')]
    history += [{'tx':t,'op':'read','key':'a','value':0} for t in ('a','b')]
    for t,tx in [('a',a),('b',b)]:
        assert e.update_by_id_tx(tx,'items','a',setv(n,1)).success
        assert e.commit_transaction(tx).success
        history += [{'tx':t,'op':'write','key':'a','value':1},{'tx':t,'op':'commit'}]
    assert check_read_committed({'a':0},history)['valid']
    assert not check_strict_serializable({'a':0},history)['valid']
    assert json.loads(e.find_by_id('items','a').data)['v']==1  # allowed lost application increment


def test_write_skew_is_allowed_under_declared_read_committed(db):
    n,e=db
    assert e.insert_many('items',['{"_id":"a","v":1}','{"_id":"b","v":1}']).success
    a=e.begin_transaction();b=e.begin_transaction()
    history=[{'tx':t,'op':'begin'} for t in ('a','b')]
    for t,tx in [('a',a),('b',b)]:
        for key in ('a','b'):
            observed=value(e,tx,key);assert observed==1
            history.append({'tx':t,'op':'read','key':key,'value':observed})
    for t,tx in [('a',a),('b',b)]:
        assert e.update_by_id_tx(tx,'items',t,setv(n,0)).success
        assert e.commit_transaction(tx).success
        history += [{'tx':t,'op':'write','key':t,'value':0},{'tx':t,'op':'commit'}]
    assert check_read_committed({'a':1,'b':1},history)['valid']
    assert not check_strict_serializable({'a':1,'b':1},history)['valid']


def test_point_lock_prevents_stale_read_and_requires_whole_transaction_retry(db):
    n,e=db
    assert e.insert_one('items','{"_id":"a","v":0}').success
    a=e.begin_transaction();b=e.begin_transaction()
    assert value(e,a,'a',True)==0
    blocked=e.find_by_id_tx(b,'items','a',for_update=True)
    assert not blocked.success and blocked.error_code=='transaction_timeout'
    assert e.rollback_transaction(b).success
    assert e.update_by_id_tx(a,'items','a',setv(n,1)).success
    assert e.commit_transaction(a).success
    retry=e.begin_transaction()
    assert value(e,retry,'a',True)==1
    assert e.update_by_id_tx(retry,'items','a',setv(n,2)).success
    assert e.commit_transaction(retry).success


def test_snapshot_pages_do_not_include_concurrent_insert_but_new_scan_does(db):
    n,e=db
    assert e.insert_many('items',['{"_id":"a","v":0}','{"_id":"c","v":0}']).success
    first=e.find_page('items',limit=1);assert first.success and first.continuation_token
    assert e.insert_one('items','{"_id":"b","v":0}').success
    second=e.find_page('items',limit=10,continuation_token=first.continuation_token)
    assert second.success and not second.continuation_token
    assert {d['_id'] for d in json.loads(first.data)+json.loads(second.data)}=={'a','c'}
    assert {d['_id'] for d in json.loads(e.find_page('items').data)}=={'a','b','c'}


def test_unique_conflict_and_retry_after_owner_abort(db):
    n,e=db
    index=n.IndexDefinition();index.index_name='u';index.fields=['v'];index.type=n.IndexType.Unique
    assert e.create_index('items',index).success
    a=e.begin_transaction();b=e.begin_transaction()
    assert e.insert_one_tx(a,'items','{"_id":"a","v":1}').success
    result=e.insert_one_tx(b,'items','{"_id":"b","v":1.0}')
    assert not result.success and result.error_code=='transaction_timeout'
    assert e.rollback_transaction(b).success
    assert e.rollback_transaction(a).success
    assert e.insert_one('items','{"_id":"b","v":1.0}').success
    assert not e.insert_one('items','{"_id":"a","v":1}').success


def test_deadlock_or_timeout_aborts_without_partial_commit(db):
    n,e=db
    assert e.insert_many('items',['{"_id":"a","v":0}','{"_id":"b","v":0}']).success
    barrier=threading.Barrier(2,timeout=3)
    def work(first,second):
        tx=e.begin_transaction()
        try:
            assert e.find_by_id_tx(tx,'items',first,for_update=True).success
            barrier.wait()
            result=e.find_by_id_tx(tx,'items',second,for_update=True)
            if not result.success:
                assert result.error_code in ('transaction_deadlock','transaction_timeout','transaction_conflict')
                assert e.rollback_transaction(tx).success
                return 'aborted'
            assert e.update_by_id_tx(tx,'items',first,setv(n,1)).success
            assert e.commit_transaction(tx).success
            return 'committed'
        finally:
            if tx.is_valid(): e.rollback_transaction(tx)
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures=[pool.submit(work,'a','b'),pool.submit(work,'b','a')]
        result=[f.result(timeout=5) for f in futures]
    assert 'aborted' in result


def setup_fk(n,e,field='_id'):
    assert e.create_collection('parents').success
    if field!='_id':
        index=n.IndexDefinition();index.index_name='parent_value';index.fields=[field]
        assert e.create_index('parents',index).success
    fk=n.ForeignKeyDefinition();fk.fk_name='p';fk.local_field='parent';fk.ref_collection='parents';fk.ref_field=field
    assert e.add_foreign_key('items',fk).success
    assert e.insert_one('parents','{"_id":"p","code":"x"}').success


@pytest.mark.parametrize('first',['child','parent'])
@pytest.mark.parametrize('field',['_id','code'])
def test_child_insert_parent_delete_interleavings_are_restrict(db,first,field):
    n,e=db;setup_fk(n,e,field)
    child=json.dumps({'_id':'child','parent':'p' if field=='_id' else 'x'})
    a=e.begin_transaction();b=e.begin_transaction()
    if first=='child':
        assert e.insert_one_tx(a,'items',child).success
        blocked=e.delete_by_id_tx(b,'parents','p')
    else:
        assert e.delete_by_id_tx(a,'parents','p').success
        blocked=e.insert_one_tx(b,'items',child)
    assert not blocked.success and blocked.error_code=='transaction_timeout'
    assert e.rollback_transaction(b).success and e.commit_transaction(a).success
    if first=='child':
        assert not e.delete_by_id('parents','p').success
        assert e.find_by_id('parents','p').success
        assert e.delete_by_id('items','child').success
        assert e.delete_by_id('parents','p').success
    else:
        assert not e.insert_one('items',child).success
    assert not e.drop_collection('parents').success  # constraint must explicitly be dropped


def test_non_id_parent_update_and_supporting_index_drop_restrict(db):
    n,e=db;setup_fk(n,e,'code')
    assert e.insert_one('items','{"_id":"child","parent":"x"}').success
    assert not e.update_by_id('parents','p',n.UpdateSpec().set('code','y')).success
    assert not e.delete_by_id('parents','p').success
    assert not e.drop_index('parents','parent_value').success


def test_constraint_installation_audits_existing_rows(db):
    n,e=db
    assert e.create_collection('parents').success
    assert e.insert_one('items','{"_id":"orphan","parent":"absent"}').success
    fk=n.ForeignKeyDefinition();fk.fk_name='p';fk.local_field='parent';fk.ref_collection='parents';fk.ref_field='_id'
    assert not e.add_foreign_key('items',fk).success
    assert not e.get_foreign_keys('items')


def test_bson_ingest_does_not_bypass_fk_and_bulk_rolls_back(db):
    from bson import BSON
    n,e=db;setup_fk(n,e)
    result=e.insert_many_bson_ingest('items',[BSON.encode({'_id':'a','parent':'p'}),
                                            BSON.encode({'_id':'b','parent':'absent'})])
    assert not result.success and e.get_collection_size('items')==0
    assert e.insert_many_bson_ingest('items',[BSON.encode({'_id':'a','parent':'p'})]).success
    assert not e.delete_by_id('parents','p').success


def test_fk_guard_is_recovered_after_reopen(db,tmp_path):
    n,_=db
    path=str(tmp_path/'reopen')
    engine=n.DocEngine(path)
    assert engine.create_collection('items').success
    setup_fk(n,engine)
    assert engine.prepare_shutdown().success
    del engine
    engine=n.DocEngine(path)
    a=engine.begin_transaction();b=engine.begin_transaction()
    try:
        assert engine.insert_one_tx(a,'items','{"_id":"child","parent":"p"}').success
        assert not engine.delete_by_id_tx(b,'parents','p').success
    finally:
        for tx in (a,b):
            if tx.is_valid(): assert engine.rollback_transaction(tx).success
        assert engine.prepare_shutdown().success


def test_document_identity_cannot_change_or_be_removed(db):
    n,e=db
    assert e.insert_one('items','{"_id":"a","v":1}').success
    for spec in (n.UpdateSpec().set('_id','b'),n.UpdateSpec().unset('_id'),n.UpdateSpec().rename('v','_id')):
        assert not e.update_by_id('items','a',spec).success
    assert json.loads(e.find_by_id('items','a').data)=={'_id':'a','v':1}


def test_history_checker_rejects_malformed_and_impossible_histories():
    with pytest.raises(ValueError): check_read_committed({},[{'tx':'x','op':'commit'}])
    with pytest.raises(ValueError): check_read_committed({},[{'tx':'x','op':'begin'}])
    assert not check_read_committed({'x':0},[{'tx':'a','op':'begin'},
        {'tx':'a','op':'read','key':'x','value':1},{'tx':'a','op':'abort'}])['valid']
    serial=[{'tx':'a','op':'begin'},{'tx':'a','op':'read','key':'x','value':0},
            {'tx':'a','op':'write','key':'x','value':1},{'tx':'a','op':'commit'},
            {'tx':'b','op':'begin'},{'tx':'b','op':'read','key':'x','value':1},{'tx':'b','op':'commit'}]
    assert check_strict_serializable({'x':0},serial)['valid']


def test_versioned_history_corpus():
    paths=list((ROOT/'tests/corpus/transaction_histories').glob('*.json'))
    assert paths
    for path in paths:
        history=json.loads(path.read_text())
        assert check_read_committed(history['initial'],history['events'])['valid']==history['expected']['read_committed']
        assert check_strict_serializable(history['initial'],history['events'])['valid']==history['expected']['strict_serializable']
