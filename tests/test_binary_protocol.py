"""Owned packed buffers and binary HTTP share the existing query safety contract."""
import json
from pathlib import Path
import struct
import sys
import os
import subprocess

import httpx
import pytest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'NexoraDB/src'))
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import load_native_module


@pytest.fixture
def database(tmp_path):
    path=ROOT/'nexoradb.cpython-310-x86_64-linux-gnu.so'
    if not path.exists(): pytest.skip('Linux Release binary required')
    native=load_native_module(AdminApiSettings(native_module_path=path))
    engine=native.DocEngine(str(tmp_path/'db'))
    assert engine.create_collection('items').success
    yield native,engine,tmp_path
    assert engine.prepare_shutdown().success


def test_packed_batch_validation_rollback_and_ownership(database):
    from bson import BSON
    _,engine,_=database
    packed=b''.join(BSON.encode({'_id':str(i),'nested':[i,{'text':'فارسی'}]}) for i in range(3))
    assert engine.insert_many_bson_buffer('items',packed).success
    del packed
    assert json.loads(engine.find_by_id('items','2').data)['nested'][0]==2
    for bad in (b'',b'\x00',struct.pack('<I',0xffffffff),BSON.encode({'_id':'new'})+b'x'):
        with pytest.raises(ValueError): engine.insert_many_bson_buffer('items',bad)
    assert not engine.find_by_id('items','new').success
    assert not engine.insert_many_bson_buffer('items',BSON.encode({'_id':'new'})+BSON.encode({'_id':'0'})).success
    assert not engine.find_by_id('items','new').success
    with pytest.raises(TypeError): engine.insert_many_bson_buffer('items',bytearray(BSON.encode({'_id':'x'})))


def test_binary_query_auth_replay_and_revocation(database):
    from bson import BSON
    from fastapi import FastAPI
    from fastapi.testclient import TestClient
    from nexoradb.api.app import create_api_router
    from nexoradb.api.security import create_app_token
    _,engine,tmp=database
    app=FastAPI();app.state.settings=settings=AdminApiSettings(db_path=tmp/'db')
    app.include_router(create_api_router(engine_provider=lambda:engine,graph_manager_provider=lambda:None))
    token=create_app_token(app_id='binary',app_name='binary',secret=settings.api_token_secret,
                           scopes=['query:execute','documents:read','documents:write'],
                           collections={'items':['read','write']})
    assert engine.create_internal_app_token(token.tokenId,'{}').success
    headers={'Authorization':'Bearer '+token.token,'Content-Type':'application/bson','Idempotency-Key':'binary-one'}
    envelope={'query':'INSERT INTO items VALUES ($doc);','parameters':{'doc':{'_id':'one','text':'فارسی'}}}
    with TestClient(app) as client:
        assert client.post('/api/v1/query/bson',content=BSON.encode(envelope)).status_code==401
        first=client.post('/api/v1/query/bson',content=BSON.encode(envelope),headers=headers)
        assert first.status_code==200,first.text
        assert not first.json().get('statementErrors')
        replay=client.post('/api/v1/query',json=envelope,headers={k:v for k,v in headers.items() if k!='Content-Type'})
        assert replay.json()==first.json()
        assert engine.get_collection_size('items')==1
        assert client.post('/api/v1/query/bson',content=b'broken',headers=headers).status_code==400
        assert client.post('/api/v1/query/bson',json=envelope,headers={'Authorization':headers['Authorization']}).status_code==415
        denied={'query':'SELECT * FROM secret;'}
        assert client.post('/api/v1/query/bson',content=BSON.encode(denied),
                           headers={k:v for k,v in headers.items() if k!='Idempotency-Key'}).status_code==403
        # Duplicate command keys must not be silently overwritten by a decoder.
        one=BSON.encode({'query':'COUNT FROM items;'})[4:-1]
        duplicate=struct.pack('<I',len(one)*2+5)+one+one+b'\0'
        assert client.post('/api/v1/query/bson',content=duplicate,headers=headers).status_code==400
        from datetime import datetime
        typed={'query':'SELECT * FROM items WHERE value = $v;', 'parameters':{'v':datetime(2026,1,1)}}
        assert client.post('/api/v1/query/bson',content=BSON.encode(typed),headers=headers).status_code==400
        assert engine.delete_internal_app_token(token.tokenId).success
        assert client.post('/api/v1/query/bson',content=BSON.encode(envelope),headers=headers).status_code==401


def test_driver_binary_request_keeps_idempotency_header():
    from bson import BSON
    from nexoradb.api.driver import NexoraDBClient
    calls=[]
    def handler(request):
        calls.append(request)
        assert request.url.path=='/api/v1/query/bson'
        assert request.headers['Content-Type']=='application/bson'
        assert request.headers['Idempotency-Key']=='batch-1'
        assert '_idempotency_key' not in BSON(request.content).decode()
        assert BSON(request.content).decode()['parameters']['doc_0']=={'_id':'one'}
        return httpx.Response(200,json={'columns':[],'rows':[],'raw':None,'executionTimeMs':0})
    with NexoraDBClient(url='https://database.test',token='test') as client:
        client._client.close();client._client=httpx.Client(transport=httpx.MockTransport(handler))
        client.insert_many('items',[{'_id':'one'}],binary=True,idempotency_key='batch-1')
    assert len(calls)==1


@pytest.mark.parametrize('ingest',[False,True])
def test_durable_packed_batch_survives_abrupt_process_exit(tmp_path,ingest):
    if not (ROOT/'nexoradb.cpython-310-x86_64-linux-gnu.so').exists():
        pytest.skip('Linux Release binary required')
    source='''import sys,os
import nexoradb
from bson import BSON
settings=nexoradb.RocksSettings()
settings.durability=nexoradb.DocumentDurability.Durable
engine=nexoradb.DocEngine(sys.argv[1],rocks_settings=settings)
if sys.argv[2]=='write':
    assert engine.create_collection('items').success
    encoded=[BSON.encode({'_id':str(i),'value':i}) for i in range(10)]
    result=(engine.insert_many_bson_ingest('items',encoded) if sys.argv[3]=='ingest'
            else engine.insert_many_bson_buffer('items',b''.join(encoded)))
    assert result.success
    print('ACK',flush=True)
    os._exit(0)
assert engine.get_collection_size('items')==10
for i in range(10): assert engine.find_by_id('items',str(i)).success
'''
    for action in ('write','recover'):
        result=subprocess.run([sys.executable,'-c',source,str(tmp_path/'crash-db'),action,'ingest' if ingest else 'normal'],
                              cwd=ROOT,env=dict(os.environ,PYTHONPATH=str(ROOT)),
                              capture_output=True,text=True,timeout=15)
        assert result.returncode==0,result.stderr
        if action=='write': assert 'ACK' in result.stdout


def test_async_driver_offloads_and_finishes_cancelled_inflight_work(monkeypatch):
    import anyio
    import threading
    import time
    from nexoradb.api.driver import NexoraDBClient
    async def check():
        owner=threading.get_ident();calls=[]
        with NexoraDBClient(url='http://localhost',token='test') as client:
            entered=threading.Event()
            def execute(*args,**kwargs):
                assert threading.get_ident()!=owner
                entered.set();time.sleep(.05);calls.append(kwargs);return 'done'
            monkeypatch.setattr(client,'execute',execute)
            async with anyio.create_task_group() as group:
                group.start_soon(client.aexecute,'COUNT FROM items;')
                while not entered.is_set(): await anyio.sleep(.001)
                # Cancelling the task group must not orphan its thread.
                group.cancel_scope.cancel()
            assert calls==[{}]
    anyio.run(check)
