"""Opt-in bounded native insert coalescing. Submission is NOT a durable ACK."""
from collections import deque
from concurrent.futures import Future
from dataclasses import dataclass
import json
import math
import threading
import time


class BatchQueueFull(RuntimeError):
    """Not admitted; this submission was not written."""


class BatchWriteError(RuntimeError):
    """Batch failed. Reconcile IDs before retrying an attempted native commit."""
    def __init__(self, message, *, outcome_unknown):
        super().__init__(message)
        self.outcome_unknown = outcome_unknown


@dataclass(frozen=True)
class DurableReceipt:
    document_id: str
    group_records: int


class DurableBatchWriter:
    """One collection, insert-only, one native worker, bounded owned BSON.

    Future.result() success is the durable ACK; enqueue/flush timeout is not.
    A validation failure rejects the whole combined batch. No retry, fallback,
    transaction merging or cross-principal authorization is performed here.
    Drain/close before shutting down the engine. Requires nexoradb[binary].
    """
    def __init__(self, engine, collection, *, max_records=128, max_bytes=1024**2,
                 max_delay_ms=1.0, queue_records=256, queue_bytes=4*1024**2):
        for value, low, high in ((max_records,1,1000),(max_bytes,256,16*1024**2),
                                 (queue_records,1,10000),(queue_bytes,256,64*1024**2)):
            if type(value) is not int or not low <= value <= high:
                raise ValueError('Invalid bounded batching limits')
        if (type(max_delay_ms) not in (int,float) or not math.isfinite(max_delay_ms)
                or not 0 <= max_delay_ms <= 2 or queue_records < max_records or queue_bytes < max_bytes):
            raise ValueError('Delay must be 0..2ms and queue must fit one maximum batch')
        if type(collection) is not str or not collection or collection.startswith('__nexora_'):
            raise ValueError('Invalid collection')
        report=json.loads(engine.durability_report())
        if not (report.get('sync_on_ack') is True and report.get('wal_enabled') is True
                and report.get('manual_wal_flush') is False):
            raise ValueError('DurableBatchWriter requires effective Durable settings')
        from bson import BSON
        self._encode=BSON.encode
        self._engine,self._collection=engine,collection
        self._max_records,self._max_bytes=max_records,max_bytes
        self._delay=max_delay_ms/1000
        self._queue_records,self._queue_bytes=queue_records,queue_bytes
        self._condition=threading.Condition()
        self._jobs=deque()
        self._records=self._bytes=0
        self._closing=False
        self._failed=False
        self._thread=threading.Thread(target=self._run,name='nexora-durable-batcher',daemon=True)
        self._thread.start()

    def submit(self, document):
        if type(document) is not dict or type(document.get('_id')) is not str or not document['_id']:
            raise ValueError('Explicit nonempty string _id required for reconciliation')
        encoded=self._encode(document) # Owned immutable copy; no producer buffer borrowed.
        if len(encoded)>self._max_bytes:
            raise ValueError('Document exceeds configured batch byte limit')
        future=Future()
        with self._condition:
            if self._closing or self._failed:
                raise RuntimeError('Batch writer is closed or failed')
            if self._records>=self._queue_records or self._bytes+len(encoded)>self._queue_bytes:
                raise BatchQueueFull('Queue capacity exhausted; submission was not written')
            self._records+=1; self._bytes+=len(encoded)
            self._jobs.append((encoded,document['_id'],future,time.monotonic()))
            self._condition.notify_all()
        return future

    def _run(self):
        while True:
            with self._condition:
                while not self._jobs and not self._closing:
                    self._condition.wait()
                if not self._jobs: return
                deadline=self._jobs[0][3]+self._delay
                while not self._closing and len(self._jobs)<self._max_records:
                    remaining=deadline-time.monotonic()
                    if remaining<=0: break
                    self._condition.wait(remaining)
                batch=[];size=0
                while self._jobs and len(batch)<self._max_records:
                    job=self._jobs[0]
                    if size+len(job[0])>self._max_bytes: break
                    batch.append(self._jobs.popleft());size+=len(job[0])
            live=[job for job in batch if job[2].set_running_or_notify_cancel()]
            error=None
            if live:
                try:
                    result=self._engine.insert_many_bson_buffer(self._collection,b''.join(job[0] for job in live))
                    if not result.success:
                        raise RuntimeError(result.error_msg)
                except Exception as exc:
                    error=BatchWriteError(str(exc),outcome_unknown=True)
            with self._condition:
                self._records-=len(batch);self._bytes-=size
                abandoned=[]
                if error:
                    self._failed=True;self._closing=True
                    abandoned=list(self._jobs);self._jobs.clear()
                    self._records-=len(abandoned);self._bytes-=sum(len(job[0]) for job in abandoned)
                self._condition.notify_all()
            for _,identifier,future,_ in live:
                if error: future.set_exception(error)
                else: future.set_result(DurableReceipt(identifier,len(live)))
            for _,_,future,_ in abandoned:
                if future.set_running_or_notify_cancel():
                    future.set_exception(BatchWriteError('Writer failed before this queued request was attempted',outcome_unknown=False))
            if error: return

    def close(self, timeout=None):
        if threading.current_thread() is self._thread:
            raise RuntimeError('Do not close writer from its Future callback')
        with self._condition:
            self._closing=True
            self._condition.notify_all()
        self._thread.join(timeout)
        if self._thread.is_alive():
            raise TimeoutError('Commit still in progress; do not shut down engine or assume writes failed')

    def stats(self):
        with self._condition:
            return {'owned_records':self._records, 'owned_bson_bytes':self._bytes,
                    'queued_records':len(self._jobs), 'inflight_records':self._records-len(self._jobs),
                    'capacity_records':self._queue_records, 'capacity_bytes':self._queue_bytes,
                    'max_group_records':self._max_records, 'max_group_bytes':self._max_bytes,
                    'max_coalescing_delay_ms':self._delay*1000,
                    'closing':self._closing, 'failed':self._failed}

    def __enter__(self): return self
    def __exit__(self,*exc): self.close()
