"""Syntax cache isolation, real native controls and administrator-only API tests."""
from concurrent.futures import ThreadPoolExecutor
import importlib
import json
from pathlib import Path
import sys
import subprocess
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))
from nexoradb_admin.query_runner import _load_nexoraql_package, execute_query, QueryExecuteRequest


@pytest.fixture
def parser():
    package = _load_nexoraql_package()
    module = importlib.import_module(package.__name__ + '.parser')
    module.configure_parser_cache(True, clear=True)
    yield package, module
    module.configure_parser_cache(True, clear=True)


def test_syntax_hit_binds_fresh_parameters_and_ast(parser):
    package, module = parser
    query = 'SELECT * FROM users WHERE age > $minimum LIMIT 10;'
    first = package.parse(query, {'minimum': 18})
    second = package.parse(query, {'minimum': 60})
    assert first[0].where.value == 18
    assert second[0].where.value == 60
    first[0].where.value = 'mutated'
    assert package.parse(query, {'minimum': 42})[0].where.value == 42
    assert module.parser_cache_stats()['entries'] == 1
    assert module.parser_cache_stats()['hits'] >= 2
    with pytest.raises(package.NexoraQLError):
        package.parse(query, {'minimum': float('nan')})
    with pytest.raises(package.NexoraQLError):
        package.parse(query)


def test_syntax_cache_bounds_errors_disable_and_concurrent_parameters(parser):
    package, module = parser
    for _ in range(2):
        with pytest.raises(package.NexoraQLError):
            package.parse('SELECT @@@;')
    assert module.parser_cache_stats()['entries'] == 0
    query = 'SELECT * FROM users WHERE age = $age;'
    with ThreadPoolExecutor(max_workers=8) as pool:
        values = list(pool.map(lambda i: package.parse(query, {'age': i})[0].where.value, range(32)))
    assert values == list(range(32))
    module.configure_parser_cache(False)
    package.parse(query, {'age': 5})
    assert module.parser_cache_stats()['entries'] == 0
    cache_type = importlib.import_module(package.__name__ + '.syntax_cache').SyntaxCache
    cache = cache_type(max_entries=2, max_bytes=100000)
    for i in range(10):
        cache.get_or_build(str(i), lambda: ['syntax'])
    assert cache.stats()['entries'] == 2
    assert cache.stats()['evictions'] == 8
    assert cache.stats()['accounted_bytes'] <= 100000


def test_cache_never_skips_authorization(parser):
    from fastapi import HTTPException
    package, _ = parser
    query = 'SET PERFORMANCE parser_cache = false;'
    package.parse(query)
    engine = SimpleNamespace()
    for _ in range(2):
        with pytest.raises(HTTPException) as caught:
            execute_query(engine=engine, graph_manager=None, payload=QueryExecuteRequest(query=query),
                          granted_scopes=frozenset({'query:execute','collections:write','monitoring:read'}))
        assert caught.value.status_code == 403
    with pytest.raises(HTTPException) as caught:
        execute_query(engine=engine, graph_manager=None, payload=QueryExecuteRequest(query=query),
                      allow_performance_controls=False)
    assert caught.value.status_code == 403


@pytest.fixture
def native_engine(tmp_path):
    from nexoradb_admin.native import load_native_module
    from nexoradb_admin.config import AdminApiSettings
    path = ROOT / 'nexoradb.cpython-310-x86_64-linux-gnu.so'
    if not path.exists():
        pytest.skip('Linux Release extension not built')
    native = load_native_module(AdminApiSettings(native_module_path=path))
    if not hasattr(native.DocEngine, 'query_tuning'):
        pytest.skip('Rebuild native query controls')
    engine = native.DocEngine(str(tmp_path / 'db'))
    assert engine.create_collection('users').success
    assert engine.insert_many('users', [json.dumps(dict(_id=str(i),age=i,status='active')) for i in range(20)]).success
    yield engine, native
    assert engine.prepare_shutdown().success


def test_query_controls_advice_and_constraints(parser, native_engine):
    package, module = parser
    engine, native = native_engine
    executor = package.Executor(engine, None)
    status = json.loads(executor.execute_text('SHOW PERFORMANCE;')[0]['data'])
    assert status['parser_cache']['enabled'] is True
    assert status['index_advisor'] is True
    for _ in range(3):
        result = executor.execute_text('SELECT * FROM users WHERE age > 10 AND status = \'active\' LIMIT 5;')
        assert result[0]['success']
    recommendations = json.loads(executor.execute_text('SHOW INDEX ADVICE;')[0]['data'])
    assert {row['field'] for row in recommendations} == {'age','status'}
    assert engine.get_indexes('users') == []  # Recommendations never execute DDL.
    index = native.IndexDefinition(); index.index_name='age_idx'; index.fields=['age']; index.type=native.IndexType.Unique
    assert engine.create_index('users',index).success
    for value in (False, True):
        flag = str(value).lower()
        assert executor.execute_text(f'SET PERFORMANCE smart_indexes = {flag};')[0]['success']
        assert executor.execute_text(f'SET PERFORMANCE read_cache_admission = {flag};')[0]['success']
        result = executor.execute_text('SELECT * FROM users WHERE age = 10 LIMIT 5;')[0]
        assert json.loads(json.dumps(result))['count'] == 1
        assert not engine.insert_one('users', json.dumps(dict(_id='dup',age=10))).success
    assert executor.execute_text('SET PERFORMANCE index_advisor = false;')[0]['success']
    assert json.loads(executor.execute_text('SHOW INDEX ADVICE;')[0]['data']) == []
    for query in ('SET PERFORMANCE parser_cache = 1;', 'SET PERFORMANCE unknown = true;'):
        with pytest.raises(package.NexoraQLError):
            executor.execute_text(query)
    assert executor.execute_text('SET PERFORMANCE parser_cache = false;')[0]['success']
    assert not module.parser_cache_stats()['enabled']
    assert executor.execute_text('SET PERFORMANCE parser_cache = true;')[0]['success']


def test_default_advisor_observes_before_any_show_or_set(parser, native_engine):
    package, _ = parser
    engine, _ = native_engine
    executor = package.Executor(engine, None)
    for _ in range(3):
        assert executor.execute_text('SELECT * FROM users WHERE age > 10 LIMIT 5;')[0]['success']
    advice = json.loads(executor.execute_text('SHOW INDEX ADVICE;')[0]['data'])
    assert [item['field'] for item in advice] == ['age']
    assert executor.execute_text('SET PERFORMANCE index_advisor = false;')[0]['success']
    for _ in range(3):
        executor.execute_text('SELECT * FROM users WHERE age > 10 LIMIT 5;')
    assert json.loads(executor.execute_text('SHOW INDEX ADVICE;')[0]['data']) == []


def test_bson_input_views_validate_and_own_committed_storage(native_engine):
    from bson import BSON
    engine, _ = native_engine
    assert engine.insert_many_bson('users', [BSON.encode({'_id': 'borrowed', 'text': 'فارسی'})]).success
    assert json.loads(engine.find_by_id('users', 'borrowed').data)['text'] == 'فارسی'
    assert not engine.insert_many_bson('users', [BSON.encode({'_id': 'rollback'}), b'bad']).success
    assert not engine.find_by_id('users', 'rollback').success
    with pytest.raises(TypeError):
        engine.insert_many_bson('users', [bytearray(BSON.encode({'_id': 'mutable'}))])


def test_admin_api_controls_require_auth_role_and_strict_booleans(native_engine):
    from fastapi.testclient import TestClient
    from nexoradb_admin.app import create_app
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.security import create_access_token
    engine, _ = native_engine
    settings = AdminApiSettings(auth_secret='a'*48)
    app = create_app(settings=settings, engine=engine)
    client = TestClient(app)
    assert client.get('/performance').status_code == 401
    viewer = create_access_token(username='reader',role='reader',settings=settings)
    assert client.get('/performance',headers={'Authorization':'Bearer '+viewer}).status_code == 403
    token = create_access_token(username='admin',role='admin',settings=settings)
    headers = {'Authorization':'Bearer '+token}
    assert client.get('/performance',headers=headers).status_code == 200
    response = client.put('/performance',headers=headers,json={'smart_indexes':False,'index_advisor':True})
    assert response.status_code == 200 and response.json()['smart_indexes'] is False
    assert client.put('/performance',headers=headers,json={'parser_cache':'false'}).status_code == 422
    assert client.put('/performance',headers=headers,json={'unknown':True}).status_code == 422
    assert client.put('/performance',headers=headers,json={'smart_indexes':True,'index_advisor':False}).status_code == 200


def test_native_blocking_operation_releases_gil(tmp_path):
    if not (ROOT / 'nexoradb.cpython-310-x86_64-linux-gnu.so').exists():
        pytest.skip('Linux native extension required')
    code = '''
import sys, threading, time
sys.path.insert(0, sys.argv[1])
import nexoradb
engine=nexoradb.DocEngine(sys.argv[2])
assert engine.create_collection('items').success
tx=engine.begin_transaction()
started=threading.Event()
results=[]
def drop():
    started.set()
    results.append(engine.drop_collection('items').success)
thread=threading.Thread(target=drop)
thread.start()
assert started.wait(2)
time.sleep(.05)
assert engine.commit_transaction(tx).success
thread.join(2)
assert not thread.is_alive() and results==[True]
assert engine.prepare_shutdown().success
'''
    completed=subprocess.run([sys.executable,'-c',code,str(ROOT),str(tmp_path/'gil_db')],
                             capture_output=True,text=True,timeout=10)
    assert completed.returncode==0,completed.stderr


def test_startup_storage_budget_is_explicit_and_validated(native_engine,tmp_path):
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import create_doc_engine
    settings=AdminApiSettings(db_path=tmp_path/'sized',rocks_memory_mb=16,rocks_cache_mb=8)
    settings.validate_for_startup()
    engine=create_doc_engine(settings)
    assert engine.rocks_metrics().cache_capacity==8*1024**2
    assert engine.prepare_shutdown().success
    for values in ({'rocks_memory_mb':3},{'rocks_memory_mb':16,'rocks_cache_mb':16},
                   {'rocks_memory_mb':True},{'rocks_cache_mb':-1}):
        with pytest.raises(RuntimeError):
            AdminApiSettings(**values).validate_for_startup()


def test_explicit_different_native_binary_is_not_silently_ignored(native_engine,tmp_path):
    from nexoradb_admin.native import _load_native_from_path
    other=tmp_path/'different.so'
    other.write_bytes(b'not the running native build')
    with pytest.raises(RuntimeError,match='different native engine'):
        _load_native_from_path(other)


def test_driver_batch_is_one_parameterized_request_not_literal_sql(parser,native_engine):
    from nexoradb.api.driver import NexoraDBClient
    package,_=parser
    engine,_=native_engine
    client=NexoraDBClient(url='http://localhost:8000',token='test-token')
    calls=[]
    def request(method,path,body):
        calls.append((method,path,body))
        return {'rows':[]}
    client._request=request
    documents=[{'_id':'batch1','value':"'); DROP COLLECTION users; --"},{'_id':'batch2','age':100}]
    client.insert_many('users',documents,idempotency_key='batch-1')
    assert len(calls)==1
    body=calls[0][2]
    assert 'DROP COLLECTION' not in body['query']
    assert body['_idempotency_key']=='batch-1'
    result=package.Executor(engine,None).execute_text(body['query'],body['parameters'])
    assert result[0]['success']
    assert json.loads(engine.find_by_id('users','batch1').data)['value']==documents[0]['value']
    for invalid in ([],[1],[{}]*1001):
        with pytest.raises(ValueError):
            client.insert_many('users',invalid)
    assert len(calls)==1
    client.close()
