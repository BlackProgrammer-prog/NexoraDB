#!/usr/bin/env python3
"""Matched read semantics across C++, pybind and authenticated real loopback TCP."""
import argparse
import json
import math
import os
from pathlib import Path
import selectors
import signal
import secrets
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))
sys.path.insert(0, str(ROOT / 'scripts'))
from research_ab import digest, exclusive_json, source_identity, host_identity, archive_sources, verify_loaded
from research_resources import ProcessSampler
from research_native import runtime_info


def make_fixture(records, operations, warmup=10):
    if not 32 <= records <= 10000 or not 10 <= operations <= 100000:
        raise ValueError('Bounded fixture requires 32..10000 records; 10..100000 operations')
    if type(warmup) is not int or not 0 <= warmup <= 100000:
        raise ValueError('warmup must be 0..100000 operations')
    documents = [{'_id': f'{i:012d}', 'username': f'user_{i}', 'value': i,
                  'nested': {'ok': True, 'region': None}, 'tags': ['research', 'v1']} for i in range(records)]
    return {'version': 2, 'warmup_operations': warmup, 'documents': documents,
            'trace': [f'{(i * 17 + 3) % records:012d}' for i in range(operations)]}


def setup_engine(fixture, path):
    import nexoradb
    if Path(path).exists():
        raise ValueError('Fresh DB path required')
    settings = nexoradb.RocksSettings()
    settings.durability = nexoradb.DocumentDurability.Durable
    settings.memory_budget_bytes = 128 * 1024**2
    engine = nexoradb.DocEngine(path, nexoradb.TransactionSettings(), settings)
    check(engine.create_collection('research_items'))
    for document in fixture['documents']:
        check(engine.insert_one('research_items', json.dumps(document)))
    return engine


def check(result):
    if not result.success:
        raise ValueError(result.error_msg)
    return result.data


def native_artifact():
    import nexoradb
    path = str(Path(nexoradb.__native_module_path__).resolve())
    return {path: digest(path)}


def measure(fixture, action, decode):
    expected = {document['_id']: document for document in fixture['documents']}
    def validate(key, response):
        rows = decode(response)
        if rows != [expected[key]]:
            raise ValueError('Full document response differs from independent fixture oracle: ' + repr(rows)[:2000])
    trace = fixture['trace']
    for step in range(fixture.get('warmup_operations', 10)):
        key = trace[step % len(trace)]
        validate(key, action(key))
    times, response_bytes = [], 0
    window = time.perf_counter()
    for key in trace:
        started = time.perf_counter_ns()
        response = action(key)
        times.append(time.perf_counter_ns() - started)
        validate(key, response)
        response_bytes += len(response)
    wall = time.perf_counter() - window
    return {'correct': True, 'errors': 0, 'operations': len(trace),
            'warmup_operations': fixture.get('warmup_operations', 10),
            'timed_call_ops_s': len(trace) * 1e9 / sum(times), 'ops_s': len(trace) / wall,
            'p99_us': sorted(times)[math.ceil(.99 * len(times)) - 1] / 1000,
            'response_bytes': response_bytes, 'oracle_in_timed_call': False,
            'oracle_in_throughput_window': True}


def binding(fixture, db):
    import nexoradb
    engine = setup_engine(fixture, db)
    try:
        # Like C++, build condition outside per-call latency; fixture/IDs shared.
        conditions = {key: nexoradb.Condition.leaf('_id', nexoradb.Op.EQ, key) for key in set(fixture['trace'])}
        result = measure(fixture, lambda key: check(engine.find_many('research_items', conditions[key], 2, 0)), json.loads)
        result.update(scope='pybind FindMany -> Python JSON string; no parser or TCP', loaded_artifacts=native_artifact(), native_build=nexoradb.build_info())
        return result
    finally:
        check(engine.prepare_shutdown())
        del engine


def serve(fixture, db):
    import uvicorn
    from fastapi import FastAPI
    from nexoradb.api.app import create_api_router
    from nexoradb.api.security import create_app_token
    from nexoradb_admin.config import AdminApiSettings
    engine = setup_engine(fixture, db)
    secret = secrets.token_urlsafe(48)
    settings = AdminApiSettings(db_path=Path(db), api_token_secret=secret, environment='test')
    app = FastAPI()
    app.state.settings = settings
    app.include_router(create_api_router(engine_provider=lambda: engine, graph_manager_provider=lambda: None))
    def token(grants):
        value = create_app_token(app_id='research', app_name='research', scopes=['query:execute', 'documents:read'], secret=secret, collections=grants, expires_in_seconds=300)
        check(engine.create_internal_app_token(value.tokenId, json.dumps({'status': 'active'})))
        return value.token
    allowed = token({'research_items': ['read']})
    denied = token({})
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    sock.listen(128)
    # Read only by the parent over the private pipe. Tokens are never persisted.
    print(json.dumps({'port': sock.getsockname()[1], 'allowed': allowed, 'denied': denied,
                      'loaded_artifacts': native_artifact()}), flush=True)
    class OwnedProbeServer(uvicorn.Server):
        @contextmanager
        def capture_signals(self):
            # Uvicorn normally re-raises SIGTERM after shutdown. This dedicated
            # probe must finish its native close receipt before process exit.
            previous = {sig: signal.signal(sig, self.handle_exit) for sig in (signal.SIGINT, signal.SIGTERM)}
            try:
                yield
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
    try:
        OwnedProbeServer(uvicorn.Config(app, log_level='error', access_log=False)).run(sockets=[sock])
    finally:
        sock.close()
        started = time.perf_counter()
        check(engine.prepare_shutdown())
        # Router closures keep an engine reference until this process exits.
        # This receipt proves PrepareShutdown, not full destructor timing.
        exclusive_json(Path(db).parent / 'server-close.json',
                       {'prepare_shutdown_success': True, 'prepare_shutdown_seconds': time.perf_counter() - started})
        del engine


def network(fixture, connection):
    import httpx
    url = f"http://127.0.0.1:{connection['port']}"
    query = {'query': 'SELECT * FROM research_items WHERE _id = $id LIMIT 2;',
             'parameters': {'id': fixture['trace'][0]}, 'include_raw': True}
    with httpx.Client(base_url=url, timeout=5, trust_env=False) as client:
        deadline = time.monotonic() + 15
        while True:
            try:
                if client.get('/api/v1/health').status_code == 200:
                    break
            except httpx.TransportError:
                pass
            if time.monotonic() > deadline:
                raise TimeoutError('Dedicated server not ready')
            time.sleep(.02)
        if client.post('/api/v1/query', json=query).status_code != 401:
            raise ValueError('Unauthenticated query was not rejected')
        forbidden = client.post('/api/v1/query', json=query, headers={'Authorization': 'Bearer ' + connection['denied']})
        if forbidden.status_code != 403 or 'user_3' in forbidden.text:
            raise ValueError(f'Collection ACL probe failed: status={forbidden.status_code}, body={forbidden.text[:1000]}')
        def action(key):
            response = client.post('/api/v1/query', json={**query, 'parameters': {'id': key}},
                                   headers={'Authorization': 'Bearer ' + connection['allowed']})
            response.raise_for_status()
            if response.http_version != 'HTTP/1.1':
                raise ValueError('Unexpected protocol')
            return response.content
        result = measure(fixture, action, decode_http_documents)
    result.update(scope='HTTP/1.1 real TCP persistent client; managed auth + ACL + NexoraQL + raw documents AND table projection serialization; no TLS',
                  authorization_checks={'missing_token': 401, 'denied_collection': 403},
                  loaded_artifacts=connection['loaded_artifacts'])
    return result


def decode_http_documents(body):
    value = json.loads(body)
    statements = value['raw']['statements']
    if value['statementErrors'] or len(statements) != 1 or statements[0].get('success') is not True:
        raise ValueError('HTTP 200 contains a failed statement')
    # `rows` is a dashboard table projection; raw preserves nested JSON types.
    return statements[0]['documents']


def run_probe(command, directory, name, stdin=None, server=None, timeout=60):
    with (directory / (name + '.stdout')).open('x') as stdout, (directory / (name + '.stderr')).open('x') as stderr:
        child = subprocess.Popen(command, stdout=stdout, stderr=stderr, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL)
        try:
            scopes = {'engine_driver_combined': [child.pid]} if server is None else {'engine_server': [server.pid], 'network_generator': [child.pid]}
            with ProcessSampler(scopes) as sampler:
                child.communicate(input=stdin, timeout=timeout)
            if child.returncode:
                raise ValueError(f'{name} failed; inspect {name}.stderr')
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
    result = json.loads((directory / (name + '.stdout')).read_text())
    if result.get('correct') is not True:
        raise ValueError('No correctness proof from ' + name)
    result['resources'] = sampler.report()
    return result


def run(directory, records=64, operations=100, cpp=ROOT / 'build/release/nexora_research_read_probe', warmup=10, timeout=60):
    if not 0 < timeout <= 600:
        raise ValueError('Layer timeout must be in (0,600] seconds')
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    fixture = make_fixture(records, operations, warmup)
    exclusive_json(directory / 'fixture.json', fixture)
    fixture_path = directory / 'fixture.json'
    cpp = Path(cpp).resolve()
    source = source_identity()
    expected_native = native_artifact()
    archive_sources(source, directory / 'source.tar.gz')
    exclusive_json(directory / 'manifest.json', {'source': source, 'host': host_identity(),
                   'fixture_sha256': digest(fixture_path), 'cpp_sha256': digest(cpp),
                   'expected_native': expected_native,
                   'source_archive_sha256': digest(directory / 'source.tar.gz'),
                   'cmake_cache_sha256': digest(cpp.parent / 'CMakeCache.txt'),
                   'driver_runtime': runtime_info(), 'comparison': 'matched result semantics, not identical code paths',
                   'publication_eligible': False})
    script = str(Path(__file__).resolve())
    reports = {}
    reports['cpp'] = run_probe([str(cpp), str(fixture_path), str(directory / 'cpp-db')], directory, 'cpp', timeout=timeout)
    reports['binding'] = run_probe([sys.executable, script, '--worker', 'binding', '--fixture', str(fixture_path), '--db', str(directory / 'binding-db')], directory, 'binding', timeout=timeout)
    server = None
    with (directory / 'server.stderr').open('x') as stderr:
        try:
            server = subprocess.Popen([sys.executable, script, '--worker', 'server', '--fixture', str(fixture_path), '--db', str(directory / 'server-db')],
                                      stdout=subprocess.PIPE, stderr=stderr, text=True)
            with selectors.DefaultSelector() as selector:
                selector.register(server.stdout, selectors.EVENT_READ)
                if not selector.select(30):
                    raise TimeoutError('Server readiness metadata timed out')
                connection = json.loads(server.stdout.readline())
            reports['http'] = run_probe([sys.executable, script, '--worker', 'network', '--fixture', str(fixture_path)], directory, 'http',
                                        stdin=json.dumps(connection).encode(), server=server, timeout=timeout)
        finally:
            if server is not None:
                server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                    raise TimeoutError('Dedicated server failed to close gracefully')
                server.stdout.close()
    close_receipt = json.loads((directory / 'server-close.json').read_text())
    if server.returncode not in (0, -15) or close_receipt.get('prepare_shutdown_success') is not True:
        raise ValueError('Dedicated server shutdown failed')
    reports['http']['server_close'] = close_receipt
    verify_loaded(reports['binding'], expected_native)
    verify_loaded(reports['http'], expected_native)
    if any(row.get('warmup_operations') != warmup for row in reports.values()):
        raise ValueError('Layer warm-up differs; rebuild the C++ probe')
    if source_identity()['source_sha256'] != source['source_sha256']:
        raise ValueError('Source changed during layer comparison')
    result = {'schema': 1, 'fixture_sha256': digest(fixture_path), 'reports': reports,
              'publication_eligible': False, 'subtraction_is_component_cost': False,
              'limitations': 'C++/binding call latency excludes oracle; throughput windows include oracle. HTTP includes parser/auth/ACL/serialization and persistent TCP, no TLS. Sampled resources cover observed child lifecycle, not exact allocation ownership. No component cost inferred by subtracting independent p99/medians.'}
    exclusive_json(directory / 'report.json', result)
    exclusive_json(directory / 'completed.json', {'sha256': {
        path.name: digest(path) for path in directory.iterdir() if path.is_file()}})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', choices=['binding', 'server', 'network'])
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--db')
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--records', type=int, default=64)
    parser.add_argument('--operations', type=int, default=100)
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args()
    if args.plan:
        fixture = make_fixture(args.records, args.operations)
        print(json.dumps({'records': len(fixture['documents']), 'operations_per_layer': len(fixture['trace']),
                          'layers': ['C++', 'pybind', 'HTTP/1.1 TCP'], 'creates_database': False}))
        return
    if args.worker:
        fixture = json.loads(args.fixture.read_text())
        if args.worker == 'server':
            serve(fixture, args.db)
            return
        result = binding(fixture, args.db) if args.worker == 'binding' else network(fixture, json.load(sys.stdin))
    else:
        if args.directory is None:
            parser.error('--directory required')
        result = run(args.directory, args.records, args.operations)
    print(json.dumps(result, allow_nan=False))


if __name__ == '__main__':
    main()
