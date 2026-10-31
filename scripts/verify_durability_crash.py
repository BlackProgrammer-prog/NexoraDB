#!/usr/bin/env python3
"""Short COR-003 process-crash check. SIGKILL only dedicated children; never power-cycles the host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))


def native_module():
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import load_native_module
    return load_native_module(AdminApiSettings())


def checked(result):
    if not result.success:
        raise RuntimeError(result.error_msg)


def worker(path, profile, entry):
    from bson import BSON
    from nexoradb_admin.config import AdminApiSettings
    from nexoradb_admin.native import create_doc_engine
    if path.exists():
        raise RuntimeError('Worker requires a new private DB path')
    native = native_module()
    if entry == 'service':
        engine = create_doc_engine(AdminApiSettings(db_path=path, rocks_auto_tune=False, rocks_memory_mb=16,
                                                  rocks_durability=profile))
    else:
        rocks = native.RocksSettings()
        rocks.memory_budget_bytes = 16*1024**2
        if profile != 'default':
            rocks.durability = getattr(native.DocumentDurability, profile.capitalize())
        engine = native.DocEngine(str(path), native.TransactionSettings(), rocks)
    engine.pause_document_migration()
    checked(engine.create_collection('items'))
    index = native.IndexDefinition()
    index.index_name, index.fields, index.type = 'key_idx', ['key'], native.IndexType.SingleField
    checked(engine.create_index('items', index))
    expected = []
    def doc(key):
        return {'_id': key, 'key': key, 'nested': {'text': 'فارسی', 'values': [1, None, True]}}
    row = doc('single'); checked(engine.insert_one('items', json.dumps(row))); expected.append(row)
    for mode in ('json', 'bson', 'exclusive'):
        rows = [doc(f'{mode}-{i:03}') for i in range(16)]
        if mode == 'json':
            checked(engine.insert_many('items', [json.dumps(row) for row in rows]))
        elif mode == 'bson':
            checked(engine.insert_many_bson_buffer('items', b''.join(BSON.encode(row) for row in rows)))
        else:
            checked(engine.insert_many_bson_ingest('items', [BSON.encode(row) for row in rows]))
        expected.extend(rows)
    update = native.UpdateSpec().set('key', 'updated-key')
    checked(engine.update_by_id('items', 'json-001', update))
    for row in expected:
        if row['_id'] == 'json-001': row['key'] = 'updated-key'
    checked(engine.delete_by_id('items', 'json-000'))
    expected = [row for row in expected if row['_id'] != 'json-000']
    tx = engine.begin_transaction()
    row = doc('committed')
    checked(engine.insert_one_tx(tx, 'items', json.dumps(row)))
    checked(engine.commit_transaction(tx)); expected.append(row)
    tx = engine.begin_transaction()
    checked(engine.insert_one_tx(tx, 'items', json.dumps(doc('rolled-back'))))
    checked(engine.rollback_transaction(tx))
    if entry == 'service':
        from fastapi import FastAPI
        from fastapi.testclient import TestClient
        from nexoradb.api.app import create_api_router
        from nexoradb.api.security import create_app_token
        app = FastAPI(); app.state.settings = AdminApiSettings(db_path=path, rocks_durability=profile)
        app.include_router(create_api_router(engine_provider=lambda:engine, graph_manager_provider=lambda:None))
        token = create_app_token(app_id='crash-test', app_name='crash-test',
            scopes=['query:execute', 'documents:write', 'monitoring:read'],
            secret=app.state.settings.api_token_secret)
        checked(engine.create_internal_app_token(token.tokenId, '{}'))
        client = TestClient(app)
        client.__enter__()  # Intentionally never exits: parent kills the entire process.
        headers = {'Authorization': 'Bearer ' + token.token}
        for binary in (False, True):
            row = doc('http-bson' if binary else 'http-json')
            payload = {'query':'INSERT INTO items VALUES ($doc);', 'parameters':{'doc':row}}
            response = (client.post('/api/v1/query/bson', content=BSON.encode(payload),
                                   headers={**headers, 'Content-Type':'application/bson'}) if binary else
                        client.post('/api/v1/query', json=payload, headers=headers))
            assert response.status_code == 200, response.text
            assert not response.json().get('statementErrors'), response.text
            expected.append(row)
        info = client.post('/api/v1/query', json={'query':'SYSTEM INFO;'}, headers=headers)
        assert info.status_code == 200 and 'sync_on_ack' in info.text, info.text
    if entry == 'queued':
        from nexoradb.batching import DurableBatchWriter
        writer = DurableBatchWriter(engine,'items',max_records=16,max_delay_ms=2)
        queued_rows=[doc(f'queued-{i:03}') for i in range(16)]
        futures=[writer.submit(row) for row in queued_rows]
        assert [future.result(timeout=5).document_id for future in futures] == [row['_id'] for row in queued_rows]
        expected.extend(queued_rows)
        # Do not close the writer or engine: parent kills with live worker thread.
    # A staging success is deliberately left uncommitted at the crash boundary.
    pending = engine.begin_transaction()
    checked(engine.insert_one_tx(pending, 'items', json.dumps(doc('uncommitted'))))
    report = json.loads(engine.durability_report())
    assert report['profile'] == ('durable' if profile == 'default' else profile)
    assert report['wal_enabled'] and not report['manual_wal_flush']
    assert report['sync_on_ack'] == (profile != 'buffered')
    metrics = json.loads(engine.operation_metrics())['write_path']
    if report['sync_on_ack']:
        assert metrics['wal_syncs'] > 0
    print('COR003_ACK ' + json.dumps({'expected':expected, 'effective':report,
          'wal_syncs':metrics['wal_syncs'], 'http_harness':'ASGI TestClient, not TCP' if entry=='service' else None}), flush=True)
    while True:
        signal.pause()  # No flush, close, shutdown or graceful exit before parent SIGKILL.


def run(directory, repetitions):
    if sys.platform != 'linux':
        raise RuntimeError('Run inside Linux/WSL; requires SIGKILL')
    directory = directory.resolve()
    if directory.is_relative_to(ROOT) and not directory.is_relative_to(ROOT / 'build'):
        raise ValueError('Use fresh build/ output or an external directory')
    directory.mkdir(parents=True, exist_ok=False)
    native = native_module()
    binary = Path(getattr(native, '__native_module_path__', native.__file__))
    manifest = {'schema':1, 'native_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                'native_path':str(binary), 'repetitions':repetitions, 'crash':'SIGKILL owned child only',
                'power_loss_tested':False, 'cases':[]}
    for entry, profile in [('native','default'), ('native','durable'), ('native','buffered'),
                           ('service','durable'), ('service','buffered'), ('queued','durable')]:
        for repetition in range(repetitions):
            name = f'{entry}-{profile}-{repetition}'
            path = directory / (name + '-db')
            with (directory / (name + '.stderr.log')).open('x') as stderr:
                child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--worker',
                    '--directory', str(path), '--profile', profile, '--entry', entry],
                    stdout=subprocess.PIPE, stderr=stderr, text=True)
                try:
                    deadline = time.monotonic() + 20
                    with selectors.DefaultSelector() as selector, (directory / (name+'.stdout.log')).open('x') as stdout:
                        selector.register(child.stdout, selectors.EVENT_READ)
                        while True:
                            if not selector.select(max(0, deadline-time.monotonic())):
                                raise TimeoutError('Missing child ACK: ' + name)
                            line = child.stdout.readline()
                            if not line:
                                raise RuntimeError('Child failed before ACK; inspect stderr: ' + name)
                            stdout.write(line); stdout.flush()
                            if line.startswith('COR003_ACK '):
                                ack = json.loads(line[len('COR003_ACK '):]); break
                            if time.monotonic() >= deadline:
                                raise TimeoutError('Missing child ACK: ' + name)
                    child.kill()
                    assert child.wait(timeout=5) == -signal.SIGKILL
                finally:
                    if child.poll() is None:
                        child.kill(); child.wait(timeout=5)
                    child.stdout.close()
            rocks = native.RocksSettings()
            rocks.memory_budget_bytes = 16*1024**2
            rocks.durability = getattr(native.DocumentDurability, ack['effective']['profile'].capitalize())
            engine = native.DocEngine(str(path), native.TransactionSettings(), rocks)
            try:
                engine.pause_document_migration()
                with engine.iter_documents('items', page_size=17) as stream:
                    actual = list(stream)
                assert sorted(actual, key=lambda row:row['_id']) == sorted(ack['expected'], key=lambda row:row['_id'])
                assert engine.get_collection_size('items') == len(actual)
                assert not engine.find_by_id('items','rolled-back').success
                assert not engine.find_by_id('items','uncommitted').success
                for row in actual:
                    condition = native.Condition.leaf('key', native.Op.EQ, row['key'])
                    result = engine.find_many('items', condition, 2)
                    checked(result)
                    assert json.loads(result.data) == [row]
                    plan = engine.explain_plan('items', condition)
                    checked(plan)
                    assert json.loads(plan.data)['scan_type'] == 'index_equality'
                for removed in ('json-000',):
                    assert not engine.find_by_id('items',removed).success
                for removed_key in ('json-000', 'json-001'):
                    result = engine.find_many('items', native.Condition.leaf('key',native.Op.EQ,removed_key),2)
                    checked(result)
                    assert json.loads(result.data) == []
                checked(engine.prepare_shutdown())
            finally:
                del engine
            manifest['cases'].append({'entry':entry, 'profile':profile, 'repetition':repetition,
                'acknowledged_documents':len(actual), 'killed':True, 'recovered':True, 'effective':ack['effective']})
            print(name + ': crash/reopen/oracle/index/count passed', flush=True)
    manifest['status'] = 'passed'
    with (directory / 'report.json').open('x') as output:
        json.dump(manifest, output, indent=2)
    return manifest


def main():
    if not __debug__:
        raise RuntimeError('Verification requires assertions enabled; do not use python -O')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--repetitions', type=int, choices=range(1,4), default=2)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--profile', choices=['default','durable','buffered'], default='durable')
    parser.add_argument('--entry', choices=['native','service','queued'], default='native')
    args = parser.parse_args()
    if args.worker:
        worker(args.directory, args.profile, args.entry)
    else:
        run(args.directory, args.repetitions)


if __name__ == '__main__':
    main()
