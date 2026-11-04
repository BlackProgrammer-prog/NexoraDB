from dataclasses import replace
import json
import os
from pathlib import Path
import sys

import pytest
from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))
from nexoradb_admin.app import create_app
from nexoradb_admin.config import AdminApiSettings
from nexoradb_admin.native import load_native_module
from nexoradb_admin.security import create_access_token


@pytest.fixture
def workspace(tmp_path):
    native = load_native_module(AdminApiSettings(native_module_path=Path(os.environ.get(
        'NEXORADB_TEST_NATIVE_PATH', ROOT / 'NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so'))))
    engine = native.DocEngine(str(tmp_path/'db'))
    settings = AdminApiSettings(auth_secret='x'*48, graph_dir=tmp_path/'graphs', db_path=tmp_path/'db')
    app = create_app(settings=settings, engine=engine)
    with TestClient(app) as client:
        assert client.post('/auth/register', json={'firstName': 'Test', 'lastName': 'Admin',
            'email': 'admin@example.test', 'password': 'StrongPass123'}).status_code == 201
        token = create_access_token(username='root', role='admin', settings=settings)
        client.headers['Authorization'] = 'Bearer ' + token
        assert engine.create_collection('items').success
        assert engine.insert_many('items', [json.dumps({'_id': f'{i:02}', 'value': i, 'secret': 'hidden'}) for i in range(5)]).success
        yield client, engine, app, settings
    assert app.state.query_cursors.entries == {}
    assert app.state.query_cursors.budget.owners == {}


def test_validation_is_non_executing_and_preserves_source_coordinates(workspace):
    client, engine, _, _ = workspace
    response = client.post('/query/validate', json={'query': 'CREATE COLLECTION never_created;'})
    assert response.status_code == 200 and response.json()['valid']
    assert not engine.collection_exists('never_created')
    query = '\n\n  SELECT *\n  FROM ;'
    invalid = client.post('/query/validate', json={'query': query}).json()
    assert not invalid['valid'] and invalid['diagnostics'][0]['line'] == 4
    executed = client.post('/query/execute', json={'query': query})
    assert executed.status_code == 400
    assert executed.json()['queryError']['line'] == 4


@pytest.mark.parametrize('query', ['SELECT * FROM items -- trailing comment',
    'SELECT * FROM items; -- trailing comment', 'SELECT * FROM items /* trailing ; comment */',
    "SELECT * FROM items WHERE secret = '--;/*';", 'SELECT * FROM items\nLIMIT 2'])
def test_validation_normalization_handles_multiline_and_comments(workspace, query):
    client, _, _, _ = workspace
    response = client.post('/query/validate', json={'query': query})
    assert response.status_code == 200 and response.json()['valid'], response.text
    assert response.json()['cursorSupported']


def test_semantic_diagnostics_and_unsupported_cursor_contract(workspace):
    client, engine, _, _ = workspace
    assert not client.post('/query/validate', json={'query': 'UPDATE items SET value = 1;'}).json()['valid']
    for query in ['CREATE COLLECTION never_created;']:
        for path in ('/query/cursors', '/query/stream'):
            response = client.post(path, json={'query': query})
            assert response.status_code == 428, response.text
    assert not engine.collection_exists('never_created')


def test_cursor_pages_share_native_snapshot_and_projection(workspace):
    client, engine, app, _ = workspace
    first = client.post('/query/cursors', json={'query': 'SELECT value FROM items;', 'pageSize': 2}).json()
    assert first['hasMore'] and first['sequence'] == 1
    assert first['rows'] == [{'_id': '00', 'value': 0}, {'_id': '01', 'value': 1}]
    assert engine.insert_one('items', '{"_id":"09","value":9}').success
    cursor_id = first['cursorId']
    second = client.post(f'/query/cursors/{cursor_id}/next', json={'sequence': 2, 'pageSize': 2}).json()
    last = client.post(f'/query/cursors/{cursor_id}/next', json={'sequence': 3, 'pageSize': 2}).json()
    assert [row['value'] for row in second['rows'] + last['rows']] == [2, 3, 4]
    assert last['totalRows'] == 5 and last['cursorId'] is None and not last['hasMore']
    assert not app.state.query_cursors.entries


def collect_cursor(client, query, *, headers=None, page_size=2):
    response = client.post('/query/cursors', json={'query': query, 'pageSize': page_size}, headers=headers)
    assert response.status_code == 200, response.text
    page, rows = response.json(), []
    while True:
        rows.extend(page['rows'])
        if not page['hasMore']: return rows, page
        response = client.post(f"/query/cursors/{page['cursorId']}/next", json={'sequence': page['sequence']+1, 'pageSize': page_size})
        assert response.status_code == 200, response.text
        page = response.json()


def test_read_script_has_separate_results_and_receipts(workspace):
    client, _, _, _ = workspace
    rows, page = collect_cursor(client, 'SELECT value FROM items LIMIT 2; COUNT FROM items; SHOW COLLECTIONS;')
    assert [r['row']['value'] for r in rows if r.get('statementType') == 'Select' and 'row' in r] == [0, 1]
    receipts = [r for r in rows if r.get('receipt')]
    assert [r['statement'] for r in receipts] == [1, 2, 3]
    assert all(r['success'] for r in receipts)
    assert not page['statementErrors']


def test_native_lookup_snapshot_and_scalar_rules(workspace):
    _, engine, _, _ = workspace
    assert engine.create_collection('targets').success
    assert engine.insert_many('targets', [json.dumps({'_id': str(i), 'key': i, 'version': 'old'}) for i in range(5)]).success
    stream = engine.iter_join_documents('items', [('targets', 'value', 'key')], page_size=1)
    try:
        assert next(stream)['__joined__']['version'] == 'old'
        assert engine.delete_by_id('targets', '1').success
        assert engine.insert_one('targets', '{"_id":"1","key":1,"version":"new"}').success
        assert next(stream)['__joined__']['version'] == 'old'
        assert [r['value'] for r in iter(lambda: next(stream, None), None)] == [2, 3, 4]
    finally:
        stream.close()
    missing = engine.iter_join_documents('items', [('targets', 'absent', 'key')], page_size=1)
    try: assert next(missing, None) is None
    finally: missing.close()


@pytest.mark.parametrize('mode', ['cursor', 'stream', 'buffered'])
def test_chained_lookup_join_projection_skip_and_limit(workspace, mode):
    client, engine, _, _ = workspace
    for name in ['targets', 'other']:
        assert engine.create_collection(name).success
        assert engine.insert_many(name, [json.dumps({'_id': str(i), 'key': i, 'label': name+str(i)}) for i in range(5)]).success
    query = 'SELECT value, __joins__.other.label FROM items LOOKUP JOIN targets ON value = key LOOKUP JOIN other ON __joined__.key = key LIMIT 2 SKIP 1;'
    if mode == 'cursor': rows, _ = collect_cursor(client, query, page_size=1)
    elif mode == 'stream':
        response = client.post('/query/stream', json={'query': query, 'pageSize': 1})
        assert response.status_code == 200, response.text
        frames = [json.loads(line) for line in response.text.splitlines()]
        assert frames[-1] == {'end': True}
        rows = [frame['document'] for frame in frames[:-1]]
    else:
        response = client.post('/query/execute', json={'query': query})
        assert response.status_code == 200, response.text
        assert not response.json()['statementErrors']
        rows = response.json()['rows']
    assert [row['value'] for row in rows] == [1, 2]
    assert all('other' in json.dumps(row) for row in rows)


def test_mutation_results_replay_without_reexecuting(workspace):
    client, engine, _, _ = workspace
    query = 'CREATE COLLECTION created_once; INSERT INTO created_once VALUES ({"_id":"a","value":1}); SELECT * FROM created_once;'
    headers = {'Idempotency-Key': 'create-once'}
    first, _ = collect_cursor(client, query, headers=headers, page_size=1)
    second, _ = collect_cursor(client, query, headers=headers, page_size=4)
    assert first == second
    assert first[-1]['executionReceipt'] and first[-1]['success']
    assert engine.get_collection_size('created_once') == 1
    conflict = client.post('/query/cursors', headers=headers, json={'query': 'SHOW COLLECTIONS; CREATE COLLECTION different;'})
    assert conflict.status_code == 409


@pytest.mark.parametrize('finish, expected', [('COMMIT', 1), ('ROLLBACK', 0)])
def test_transaction_finishes_before_first_page(workspace, finish, expected, monkeypatch):
    monkeypatch.setattr(sys.modules['nexoradb'], 'GRAPH_ENABLED', False)
    client, engine, _, _ = workspace
    query = 'BEGIN TRANSACTION; INSERT INTO items VALUES ({"_id":"tx","value":99}); '+finish+'; SELECT * FROM items WHERE _id = \'tx\';'
    response = client.post('/query/cursors', headers={'Idempotency-Key': 'tx-'+finish}, json={'query': query, 'pageSize': 1})
    assert response.status_code == 200, response.text
    assert int(engine.find_by_id('items', 'tx').success) == expected
    if response.json()['cursorId']: client.delete('/query/cursors/'+response.json()['cursorId'])


def test_preflight_invalid_late_statement_has_no_side_effect(workspace):
    client, engine, _, _ = workspace
    response = client.post('/query/cursors', headers={'Idempotency-Key': 'preflight'}, json={
        'query': 'CREATE COLLECTION not_created; BEGIN TRANSACTION; SHOW COLLECTIONS; COMMIT;'})
    assert response.status_code == 422, response.text
    assert not engine.collection_exists('not_created')


def test_failure_protocol_does_not_treat_document_fields_as_errors(workspace):
    client, engine, _, _ = workspace
    assert engine.insert_one('items', '{"_id":"failure-data","success":false}').success
    response = client.post('/query/stream', json={'query': "SELECT * FROM items WHERE _id = 'failure-data';"})
    frames = [json.loads(line) for line in response.text.splitlines()]
    assert frames[0]['document']['success'] is False
    assert frames[-1] == {'end': True}
    response = client.post('/query/stream', headers={'Idempotency-Key': 'failure-script'}, json={
        'query': 'CREATE COLLECTION prefix_kept; CREATE COLLECTION prefix_kept; CREATE COLLECTION suffix_skipped;'})
    assert response.status_code == 200, response.text
    frames = [json.loads(line) for line in response.text.splitlines()]
    assert 'error' in frames[-1] and 'end' not in frames[-1]
    assert engine.collection_exists('prefix_kept') and not engine.collection_exists('suffix_skipped')


def test_ambiguous_mutation_key_never_runs_again(workspace, tmp_path):
    from nexoradb_admin.query_program import ProgramResults
    results = ProgramResults(tmp_path/'journal.sqlite3', program_bytes=1)
    client, engine, _, _ = workspace
    from nexoradb_admin.query_tools import prepare_program
    from nexoradb_admin.query_runner import QueryExecuteRequest
    from nexoradb_admin.query_program import ProgramStream
    payload = QueryExecuteRequest(query='CREATE COLLECTION quota_prefix;')
    statements, semantic = prepare_program(payload)
    try:
        factory = lambda: ProgramStream(engine, statements, semantic)
        from fastapi import HTTPException
        with pytest.raises(HTTPException) as failure:
            results.prepare('admin', 'quota-key', {'query': payload.query}, factory)
        assert failure.value.status_code == 413
        assert engine.collection_exists('quota_prefix')
        with pytest.raises(HTTPException) as retry:
            results.prepare('admin', 'quota-key', {'query': payload.query}, factory)
        assert retry.value.status_code == 409
        assert results.used == 0
    finally: results.shutdown()


def test_journal_status_is_owner_bound_and_survives_delivery_close(workspace):
    client, _, _, settings = workspace
    response = client.post('/query/cursors', headers={'Idempotency-Key': 'receipt-key'}, json={
        'query': 'CREATE COLLECTION receipt_test;', 'pageSize': 1})
    assert response.status_code == 200, response.text
    if response.json()['cursorId']: client.delete('/query/cursors/'+response.json()['cursorId'])
    status = client.get('/query/requests/receipt-key').json()
    assert status['status'] == 'completed'
    assert status['receipts'][0]['type'] == 'CreateCollection'
    other = create_access_token(username='another-admin', role='admin', settings=settings)
    assert client.get('/query/requests/receipt-key', headers={'Authorization': 'Bearer '+other}).status_code == 401
    from nexoradb_admin.idempotency import RequestJournal
    from fastapi import HTTPException
    with pytest.raises(HTTPException) as denied:
        RequestJournal(settings.db_path/'query_requests.sqlite3').inspect('admin:another-admin', 'receipt-key')
    assert denied.value.status_code == 404


def test_expired_spool_keeps_durable_receipt_without_reexecuting(workspace):
    client, engine, app, _ = workspace
    query = 'CREATE COLLECTION expires_once;'
    headers = {'Idempotency-Key': 'expire-result'}
    collect_cursor(client, query, headers=headers)
    app.state.query_program_results.lifetime = 0
    app.state.query_program_results.reap()
    response = client.post('/query/cursors', json={'query': query}, headers=headers)
    assert response.status_code == 410, response.text
    assert engine.collection_exists('expires_once')
    assert client.get('/query/requests/expire-result').json()['status'] == 'completed'


def test_global_disk_admission_precedes_side_effects(workspace):
    client, engine, app, _ = workspace
    app.state.query_program_results.total_bytes = 0
    response = client.post('/query/cursors', headers={'Idempotency-Key': 'disk-full'}, json={
        'query': 'CREATE COLLECTION disk_not_started;'})
    assert response.status_code == 507, response.text
    assert not engine.collection_exists('disk_not_started')


def test_active_spool_readers_keep_their_disk_credit_after_expiry(workspace):
    client, _, app, _ = workspace
    response = client.post('/query/cursors', headers={'Idempotency-Key': 'active-spool'}, json={
        'query': 'CREATE COLLECTION retained_credit;', 'pageSize': 1})
    assert response.status_code == 200, response.text
    store = app.state.query_program_results
    assert store.used > 0
    before = store.used
    store.lifetime = 0
    store.reap()
    assert store.used == before and store.entries
    client.delete('/query/cursors/'+response.json()['cursorId'])
    assert store.used == 0 and not store.entries


def test_application_stream_scripts_use_scopes_replay_and_owned_receipts(workspace):
    from nexoradb.api.security import create_app_token
    client, engine, _, settings = workspace
    token = create_app_token(app_id='program-app', app_name='Program app',
        scopes=['query:execute', 'documents:read', 'documents:write'], secret=settings.api_token_secret,
        collections={'items': ['read', 'write']})
    assert engine.create_internal_app_token(token.tokenId, json.dumps({'appId': 'program-app'})).success
    headers = {'Authorization': 'Bearer '+token.token, 'Idempotency-Key': 'app-write-once'}
    query = 'INSERT INTO items VALUES ({"_id":"app-once","value":17}); SELECT * FROM items WHERE _id = \'app-once\';'
    first = client.post('/api/v1/query/cursor', json={'query': query}, headers=headers)
    assert first.status_code == 200, first.text
    frames = [json.loads(line) for line in first.text.splitlines()]
    assert frames[-1] == {'end': True}
    assert any(f.get('document', {}).get('executionReceipt') for f in frames)
    replay = client.post('/api/v1/query/cursor', json={'query': query}, headers=headers)
    assert replay.status_code == 200 and replay.text == first.text
    status = client.get('/api/v1/query/requests/app-write-once', headers=headers)
    assert status.status_code == 200 and status.json()['status'] == 'completed'
    assert client.get('/query/requests/app-write-once').status_code == 404


def test_transaction_failure_rolls_back_before_delivery(workspace, monkeypatch):
    monkeypatch.setattr(sys.modules['nexoradb'], 'GRAPH_ENABLED', False)
    client, engine, _, _ = workspace
    rows, page = collect_cursor(client,
        'BEGIN TRANSACTION; INSERT INTO items VALUES ({"_id":"dup","value":1}); '
        'INSERT INTO items VALUES ({"_id":"dup","value":2}); COMMIT;',
        headers={'Idempotency-Key': 'rollback-failure'}, page_size=500)
    assert page['statementErrors']
    assert not engine.find_by_id('items', 'dup').success
    assert rows[-1]['executionReceipt'] and not rows[-1]['success']
    assert any(r['outcome'] == 'rolled_back' for r in rows[-1]['statements'])


def test_join_snapshot_remains_correct_after_index_creation(workspace):
    client, engine, _, _ = workspace
    assert engine.create_collection('indexed_targets').success
    assert engine.insert_many('indexed_targets', [json.dumps({'_id': str(i), 'key': i}) for i in range(5)]).success
    stream = engine.iter_join_documents('items', [('indexed_targets', 'value', 'key')], page_size=1)
    try:
        assert next(stream)['value'] == 0
        response = client.post('/query/execute', json={'query': 'CREATE INDEX target_key ON indexed_targets (key);'})
        assert response.status_code == 200 and not response.json()['statementErrors'], response.text
        assert [r['value'] for r in iter(lambda: next(stream, None), None)] == [1, 2, 3, 4]
    finally: stream.close()


def test_join_can_stream_more_than_buffered_response_budget(workspace):
    client, engine, _, _ = workspace
    assert engine.create_collection('large_source').success
    assert engine.create_collection('lookup_key').success
    assert engine.insert_one('lookup_key', '{"_id":"target","label":"joined"}').success
    for start in range(0, 850, 25):
        batch = [json.dumps({'_id': str(i).zfill(4), 'target': 'target', 'payload': 'x'*20000})
                 for i in range(start, min(start+25, 850))]
        assert engine.insert_many('large_source', batch).success
    query = 'SELECT * FROM large_source LOOKUP JOIN lookup_key ON target = _id;'
    response = client.post('/query/stream', json={'query': query, 'pageSize': 16})
    assert response.status_code == 200, response.text[:200]
    assert len(response.content) > 16*1024*1024
    frames = [json.loads(line) for line in response.text.splitlines()]
    assert len(frames) == 851 and frames[-1] == {'end': True}
    assert all(frame['document']['__joined__']['label'] == 'joined' for frame in frames[:-1])


def test_cursor_replay_sequence_and_close_are_safe(workspace):
    client, _, app, _ = workspace
    first = client.post('/query/cursors', json={'query': 'SELECT * FROM items', 'pageSize': 1}).json()
    path = f"/query/cursors/{first['cursorId']}"
    assert client.post(path+'/next', json={'sequence': 3}).status_code == 409
    entry = app.state.query_cursors.entries[first['cursorId']]
    with entry.lock:
        assert client.post(path+'/next', json={'sequence': 2}).status_code == 409
    assert client.post(path+'/next', json={'sequence': 2, 'pageSize': 1}).json()['rows'][0]['value'] == 1
    assert client.post(path+'/next', json={'sequence': 2}).status_code == 409
    assert client.delete(path).status_code == 204
    assert client.delete(path).status_code == 204
    assert not app.state.query_cursors.budget.owners


def test_cursor_is_bound_to_session_and_enforces_quotas(workspace):
    client, _, app, settings = workspace
    ids = [client.post('/query/cursors', json={'query': 'SELECT * FROM items;', 'pageSize': 1}).json()['cursorId'] for _ in range(2)]
    assert client.post('/query/cursors', json={'query': 'SELECT * FROM items;'}).status_code == 429
    other = create_access_token(username='root', role='admin', settings=replace(settings, access_token_ttl_seconds=900))
    headers = {'Authorization': 'Bearer ' + other}
    assert client.post(f'/query/cursors/{ids[0]}/next', json={'sequence': 2}, headers=headers).status_code == 404
    assert client.delete(f'/query/cursors/{ids[0]}', headers=headers).status_code == 404
    assert len(app.state.query_cursors.entries) == 2
    for cursor_id in ids:
        assert client.delete(f'/query/cursors/{cursor_id}').status_code == 204


@pytest.mark.parametrize('hard', [False, True])
def test_idle_and_absolute_expiry_release_native_resources(workspace, hard):
    client, _, app, _ = workspace
    cursor_id = client.post('/query/cursors', json={'query': 'SELECT * FROM items;', 'pageSize': 1}).json()['cursorId']
    store = app.state.query_cursors
    entry = store.entries[cursor_id]
    if hard:
        entry.touched = entry.created + 120
    store.clock = lambda: entry.created + (121 if hard else 31)
    store.reap()
    assert not store.entries and not store.budget.owners
    assert client.post(f'/query/cursors/{cursor_id}/next', json={'sequence': 2}).status_code == 404


def test_streaming_honors_parameter_filter_skip_limit_projection_and_end_marker(workspace):
    client, _, _, _ = workspace
    response = client.post('/query/stream', json={
        'query': 'SELECT value FROM items WHERE value >= $min LIMIT 2 SKIP 1;',
        'parameters': {'min': 1}, 'pageSize': 1})
    assert response.status_code == 200, response.text
    assert response.headers['content-type'].startswith('application/x-ndjson')
    assert response.headers['x-accel-buffering'] == 'no'
    frames = [json.loads(line) for line in response.text.splitlines()]
    assert frames == [{'document': {'_id': '02', 'value': 2}}, {'document': {'_id': '03', 'value': 3}}, {'end': True}]


def test_idle_cursor_is_closed_on_service_shutdown(workspace):
    client, _, app, _ = workspace
    response = client.post('/query/cursors', json={'query': 'SELECT * FROM items;', 'pageSize': 1})
    assert response.status_code == 200 and app.state.query_cursors.entries
    # Fixture verifies shutdown releases the native stream and cursor budget.


def test_new_query_tools_require_authentication(workspace):
    client, _, _, _ = workspace
    client.headers.pop('Authorization')
    for path in ('/query/validate', '/query/cursors', '/query/stream'):
        assert client.post(path, json={'query': 'SELECT * FROM items;'}).status_code == 401


def test_eof_diagnostic_maps_to_original_text_not_added_terminator(workspace):
    client, _, _, _ = workspace
    query = 'SELECT * FROM'
    diagnostic = client.post('/query/validate', json={'query': query}).json()['diagnostics'][0]
    assert diagnostic['line'] == 1
    assert diagnostic['column'] == len(query) + 1


def test_stream_failure_is_incomplete_and_releases_admission(workspace, monkeypatch):
    import nexoradb_admin.query_routes as routes
    client, _, app, _ = workspace
    original = routes.require_active_admin
    calls = 0
    def authorize(engine, payload):
        nonlocal calls
        calls += 1
        if calls >= 5:  # Revoke after open, statement, scan and first-batch authorization.
            from fastapi import HTTPException
            raise HTTPException(403, 'revoked')
        return original(engine, payload)
    monkeypatch.setattr(routes, 'require_active_admin', authorize)
    response = client.post('/query/stream', json={'query': 'SELECT * FROM items;', 'pageSize': 1})
    frames = [json.loads(line) for line in response.text.splitlines()]
    assert response.status_code == 200
    assert 'document' in frames[0] and 'error' in frames[-1]
    assert not any(frame.get('end') for frame in frames)
    assert app.state.lifecycle.controller.running == []


def test_close_cancels_a_busy_native_scope_before_waiting_for_its_lock(workspace):
    from threading import Thread
    client, _, app, _ = workspace
    cursor_id = client.post('/query/cursors', json={'query': 'SELECT * FROM items;', 'pageSize': 1}).json()['cursorId']
    store = app.state.query_cursors
    entry = store.entries[cursor_id]
    with entry.lock:
        closer = Thread(target=lambda: store.close(cursor_id, entry.owner, entry.session))
        closer.start()
        assert entry.control.event.wait(2), 'Close must cancel execution before waiting for an active page'
    closer.join(2)
    assert not closer.is_alive() and not store.entries and not store.budget.owners


def test_missing_collection_stream_fails_before_successful_stream_headers(workspace):
    client, _, _, _ = workspace
    response = client.post('/query/stream', json={'query': 'SELECT * FROM missing;'})
    assert response.status_code == 400
    assert response.headers['content-type'].startswith('application/json')
    assert 'end' not in response.json()


def test_cursor_admission_does_not_misclassify_large_scans_as_small_queries(workspace, monkeypatch):
    client, _, app, _ = workspace
    controller = app.state.lifecycle.controller
    original = controller.acquire
    costs = []
    def acquire(owner, cost, **kwargs):
        costs.append(cost)
        return original(owner, cost, **kwargs)
    monkeypatch.setattr(controller, 'acquire', acquire)
    assert client.post('/query/validate', json={'query': 'SELECT * FROM items;'}).status_code == 200
    assert costs[-1] == 1
    assert client.post('/query/cursors', json={'query': 'SELECT * FROM items;'}).status_code == 200
    assert costs[-1] == controller.policy.owner_cost - controller.policy.small_cost
    assert client.post('/query/stream', json={'query': 'SELECT * FROM items;'}).status_code == 200
    assert costs[-1] == controller.policy.owner_cost - controller.policy.small_cost
