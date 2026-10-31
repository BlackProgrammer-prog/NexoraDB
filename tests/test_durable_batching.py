import json
from pathlib import Path
import sys
import threading
from types import SimpleNamespace
from concurrent.futures import ThreadPoolExecutor

import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'NexoraDB/src'))
from nexoradb.batching import DurableBatchWriter, BatchQueueFull, BatchWriteError


class Engine:
    def __init__(self):
        self.entered=threading.Event();self.release=threading.Event();self.calls=[];self.fail=False
    def durability_report(self):
        return json.dumps({'sync_on_ack':True,'wal_enabled':True,'manual_wal_flush':False})
    def insert_many_bson_buffer(self,collection,data):
        self.calls.append(data);self.entered.set()
        assert self.release.wait(3)
        return SimpleNamespace(success=not self.fail,error_msg='injected commit failure')


def test_ack_only_after_commit_and_inflight_counts_toward_capacity():
    engine=Engine()
    writer=DurableBatchWriter(engine,'items',max_records=1,queue_records=1,max_delay_ms=0)
    try:
        future=writer.submit({'_id':'one'})
        assert engine.entered.wait(1)
        assert not future.done()
        assert writer.stats()['inflight_records']==1
        with pytest.raises(BatchQueueFull): writer.submit({'_id':'two'})
        with pytest.raises(TimeoutError): writer.close(timeout=.001)
        engine.release.set()
        assert future.result(timeout=1).document_id=='one'
    finally:
        engine.release.set();writer.close()
    with pytest.raises(RuntimeError): writer.submit({'_id':'three'})
    assert writer.stats()['owned_records']==0


def test_commit_failure_poisoning_and_queued_not_attempted():
    engine=Engine();engine.fail=True
    writer=DurableBatchWriter(engine,'items',max_records=1,queue_records=2,max_delay_ms=0)
    first=writer.submit({'_id':'one'});assert engine.entered.wait(1)
    second=writer.submit({'_id':'two'})
    engine.release.set();writer.close()
    for future,unknown in ((first,True),(second,False)):
        with pytest.raises(BatchWriteError) as failed: future.result()
        assert failed.value.outcome_unknown is unknown
    assert len(engine.calls)==1


def test_cancellation_and_byte_budget_include_inflight():
    engine=Engine()
    writer=DurableBatchWriter(engine,'items',max_records=1,max_bytes=256,
                              queue_records=3,queue_bytes=512,max_delay_ms=0)
    try:
        first=writer.submit({'_id':'one','payload':'x'*150})
        assert engine.entered.wait(1)
        second=writer.submit({'_id':'two','payload':'x'*150})
        assert second.cancel()
        with pytest.raises(BatchQueueFull): writer.submit({'_id':'three','payload':'x'*150})
        engine.release.set()
        assert first.result(timeout=1).document_id=='one'
    finally:
        engine.release.set();writer.close()
    assert len(engine.calls)==1 and second.cancelled()


def test_limits_and_buffered_rejected():
    engine=Engine()
    for kwargs in ({'max_delay_ms':3},{'max_delay_ms':float('nan')},{'max_records':0},
                   {'queue_records':1},{'max_bytes':True}):
        with pytest.raises(ValueError): DurableBatchWriter(engine,'items',**kwargs)
    engine.durability_report=lambda:json.dumps({'sync_on_ack':False})
    with pytest.raises(ValueError): DurableBatchWriter(engine,'items')


def test_real_native_coalescing_atomic_failure_and_no_data_loss(tmp_path):
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import load_native_module
    native=load_native_module(AdminApiSettings())
    engine=native.DocEngine(str(tmp_path/'db'))
    assert engine.create_collection('items').success
    try:
        before=json.loads(engine.operation_metrics())['write_path']['wal_syncs']
        with DurableBatchWriter(engine,'items',max_records=64,max_delay_ms=2) as writer:
            with ThreadPoolExecutor(max_workers=4) as producers:
                groups=list(producers.map(lambda worker:[writer.submit({'_id':f'{worker}-{i}'}) for i in range(32)],range(4)))
            receipts=[future.result(timeout=3) for group in groups for future in group]
        assert len({receipt.document_id for receipt in receipts})==128
        assert engine.get_collection_size('items')==128
        syncs=json.loads(engine.operation_metrics())['write_path']['wal_syncs']-before
        assert 0<syncs<128
        # Both requests share one atomic batch. Failure cannot ACK a valid prefix.
        with DurableBatchWriter(engine,'items',max_records=2,max_delay_ms=2) as writer:
            futures=[writer.submit({'_id':'new'}),writer.submit({'_id':'0-0'})]
            for future in futures:
                with pytest.raises(BatchWriteError): future.result(timeout=3)
        assert not engine.find_by_id('items','new').success
    finally:
        assert engine.prepare_shutdown().success
