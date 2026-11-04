"""Exercise the production build with Chromium and mocked API responses.

Run from the repo root with NexoraDB/.venv/bin/python after npm run build.
Requires playwright; set NEXORADB_BROWSER_EXECUTABLE for an existing Chromium,
or install the matching browser with python -m playwright install chromium.
No real database or credentials are used.
"""
from collections import Counter
from pathlib import Path
import json
import os
import socket
import sys
import threading
import time
from urllib.parse import urlsplit

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from playwright.sync_api import sync_playwright, expect
import uvicorn

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'NexoraDB/src'))
from nexoradb_admin.browser_security import BrowserSecurityMiddleware


def main():
    app = FastAPI()
    app.mount('/admin', StaticFiles(directory=ROOT / 'NexoraDB/src/nexoradb/dashboard/static', html=True))
    app.add_middleware(BrowserSecurityMiddleware, production=False)
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(app, log_level='error'))
    thread = threading.Thread(target=lambda: server.run(sockets=[sock]), daemon=True)
    thread.start()
    for _ in range(100):
        if server.started:
            break
        time.sleep(.05)
    counts = Counter()
    unauthorized = False
    expires_in = 3600
    held_requests = []
    user = dict(_id='root', username='root', displayName='Test Admin', email='admin@example.test',
                role='admin', status='active', createdAt=1, updatedAt=1, lastLoginAt=1)
    docs = [dict(id=str(i), collectionName='users', data={'index': i},
                 createdAt='2026-11-03T00:00:00Z', updatedAt='2026-11-03T00:00:00Z') for i in range(125)]

    def api(route):
        path = urlsplit(route.request.url).path
        if path.startswith('/admin'):
            return route.continue_()
        counts[path] += 1
        if path == '/auth/setup-state':
            data = {'needsSetup': False}
        elif path == '/auth/login':
            data = {'accessToken': 'test-only-bearer', 'expiresIn': expires_in, 'tokenType': 'bearer', 'user': user}
        elif path == '/auth/me':
            data = user
        elif path == '/query/validate':
            query = route.request.post_data_json['query']
            invalid = 'FRUM' in query
            data = dict(valid=not invalid, diagnostics=[dict(message='Expected FROM', line=2, column=1, severity='error')] if invalid else [],
                        statementCount=0 if invalid else max(1, query.count(';')), cursorSupported=not invalid)
        elif path == '/query/stream':
            assert route.request.headers.get('idempotency-key')
            if route.request.post_data_json['query'].count('SELECT') > 1:
                records = [{'statement': 1, 'statementType': 'Select', 'row': {'marker': 'script-row', 'value': 17}},
                           {'statement': 1, 'statementType': 'Select', 'receipt': True, 'success': True, 'rowCount': 1},
                           {'statement': 2, 'statementType': 'Select', 'receipt': True, 'success': True, 'rowCount': 0}]
            else:
                records = [{'value': i} for i in range(2000)]
            body = ''.join(json.dumps({'document': row})+'\n' for row in records) + '{"end":true}\n'
            return route.fulfill(content_type='application/x-ndjson', body=body)
        elif path == '/query/cursors':
            data = dict(cursorId='browser-test', rows=[dict(value=i) for i in range(100)], columns=['value'], hasMore=True, sequence=1, totalRows=100)
        elif path == '/query/cursors/browser-test/next':
            data = dict(cursorId=None, rows=[dict(value=i) for i in range(100, 150)], columns=['value'], hasMore=False, sequence=2, totalRows=150)
        elif path == '/query/cursors/browser-test':
            return route.fulfill(status=204)
        elif path == '/collections':
            if unauthorized:
                return route.fulfill(status=401, json={'message': 'expired'})
            data = [dict(name=name, documentCount=125, createdAt='2026-11-03T00:00:00Z') for name in ('users', 'slow', 'events')]
        elif path == '/collections/users/documents':
            data = docs
        elif path == '/collections/slow/documents':
            held_requests.append(route)
            return
        elif path == '/collections/events/documents':
            data = [dict(docs[0], id='events-only', collectionName='events')]
        elif path == '/query/execute':
            query = route.request.post_data_json['query']
            if query == 'OVERSIZE':
                data = {'padding': 'x' * 9_000_000}
            elif query == 'REDIRECT':
                return route.fulfill(status=302, headers={'Location': f'http://127.0.0.1:{port}/leaked-token'})
            elif query == 'NON_JSON':
                return route.fulfill(content_type='text/html', body='<script>window.xss=true</script>')
            else:
                data = dict(columns=['value'], rows=[{'value': '<img src=x onerror="window.xss=true">'} for _ in range(2500)],
                            raw={'statements': [{'algo': 'BFS', 'graph': 'social'}]} if query == 'GRAPH' else {'rows': list(range(2500))},
                            executionTimeMs=1)
        elif path == '/graphs/social/visualization':
            data = dict(graphId='social', nodeCount=2, edgeCount=1, maxNodes=1000,
                        nodes=[dict(id='a', label='A'), dict(id='b', label='B')],
                        edges=[dict(id='ab', source='a', target='b')])
        elif path == '/performance':
            data = dict(parser_cache=dict(enabled=True, entries=0, hits=0, misses=0, accounted_bytes=0),
                        advice=[], native_tuning_available=False)
        else:
            return route.fulfill(status=404, json={'message': 'test route not configured'})
        route.fulfill(json=data)

    try:
        with sync_playwright() as playwright:
            executable = os.getenv('NEXORADB_BROWSER_EXECUTABLE')
            browser = playwright.chromium.launch(headless=True, **({'executable_path': executable} if executable else {}))
            page = browser.new_page(viewport={'width': 1440, 'height': 1000})
            errors = []
            csp_errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.on('console', lambda message: csp_errors.append(message.text) if 'Content Security Policy' in message.text else None)
            requests = []
            aborted = []
            page.on('request', lambda request: requests.append(request.url))
            page.on('requestfailed', lambda request: aborted.append(request.url))
            page.route('**/*', api)
            page.add_init_script("localStorage.setItem('nexoradb_admin_access_token', 'legacy-token')")
            url = f'http://127.0.0.1:{port}/admin/'
            page.goto(url)
            expect(page.get_by_role('button', name='Sign in', exact=True)).to_be_enabled()
            assert not any('GraphVisualization-' in value or 'DashboardPage-' in value for value in requests)
            assert page.evaluate("localStorage.getItem('nexoradb_admin_access_token')") is None
            assert page.locator('img[alt="NexoraDB"]').evaluate_all('(images) => images.every(img => img.complete && img.naturalWidth > 0)')

            def login():
                page.get_by_label('Password', exact=True).fill('test-only-password')
                page.get_by_role('button', name='Sign in', exact=True).click()
                expect(page.get_by_role('button', name='Sign out', exact=True)).to_be_visible()

            login()
            logo = page.locator('aside img[alt="NexoraDB"]')
            expect(logo).to_have_attribute('src', '/admin/brand/nexoradb-logo.svg')
            assert logo.evaluate('(img) => img.complete && img.naturalWidth > 0')
            assert page.evaluate('Object.keys(localStorage).length') == 0
            page.get_by_role('link', name='Documents', exact=True).click()
            expect(page.get_by_text('1–50 of 125', exact=True)).to_be_visible()
            assert counts['/collections'] == 1 and counts['/collections/users/documents'] == 1, counts
            assert page.get_by_role('button', name='Edit', exact=True).count() == 50
            page.get_by_role('button', name='Edit', exact=True).first.click()
            page.locator('textarea').fill('{"unsaved": true}')
            page.get_by_role('button', name='Close modal').click()
            page.get_by_role('button', name='Edit', exact=True).first.click()
            assert json.loads(page.locator('textarea').input_value()) == {'index': 0}
            page.get_by_role('button', name='Close modal').click()
            page.get_by_role('button', name='Next', exact=True).click()
            expect(page.get_by_text('51–100 of 125', exact=True)).to_be_visible()
            page.get_by_role('button', name='slow', exact=True).click()
            expect(page.get_by_text('Loading documents', exact=True)).to_be_visible()
            page.get_by_role('button', name='events', exact=True).click()
            expect(page.get_by_role('heading', name='events-only', exact=True)).to_be_visible()
            assert any('/collections/slow/documents' in value for value in aborted), aborted
            for held in held_requests:
                held.fulfill(json=docs)
            held_requests.clear()
            expect(page.get_by_role('heading', name='events-only', exact=True)).to_be_visible()

            page.get_by_role('link', name='Query editor', exact=True).click()
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_text('1–50 of 2500', exact=True)).to_be_visible()
            assert page.locator('tbody tr').count() == 50
            assert page.evaluate('window.xss') is None
            expect(page.get_by_role('button', name='Show JSON preview')).to_be_visible()
            assert not any('GraphVisualization-' in value for value in requests)
            page.get_by_role('button', name='Show JSON preview').click()
            expect(page.get_by_text('Bounded preview;', exact=False)).to_be_visible()
            page.get_by_label('Query', exact=True).fill('REDIRECT')
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_text('Failed to fetch', exact=False).first).to_be_visible()
            assert counts['/leaked-token'] == 0
            page.get_by_label('Query', exact=True).fill('NON_JSON')
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_text('API returned a non-JSON response', exact=False).first).to_be_visible()
            assert page.evaluate('window.xss') is None
            page.get_by_label('Query', exact=True).fill('OVERSIZE')
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_text('Response exceeds the console 8 MiB limit.', exact=False).first).to_be_visible()
            page.get_by_label('Query', exact=True).fill('GRAPH')
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_text('Interactive graph', exact=True)).to_be_visible()
            expect(page.locator('canvas').first).to_be_visible()
            assert any('GraphVisualization-' in value for value in requests)

            editor = page.get_by_label('Query', exact=True)
            editor.fill('SELECT *\nFRUM users;')
            expect(page.get_by_text('Line 2, column 1: Expected FROM', exact=True)).to_be_visible()
            expect(page.get_by_role('button', name='Run Query', exact=True)).to_be_disabled()
            expect(page.locator('.cm-lintRange-error').first).to_be_visible()
            editor.fill('SELECT *\nFROM users;')
            expect(page.get_by_text('Syntax and semantic checks passed.', exact=False)).to_be_visible()
            assert page.locator('.cm-line').count() >= 2
            assert page.locator('.cm-line span').evaluate_all("(spans) => spans.some(span => span.textContent === 'SELECT' && getComputedStyle(span).color !== getComputedStyle(span.closest('.cm-content')).color)")
            editor.press('Control+Enter')
            expect(page.get_by_text('1–50 of 2500', exact=True)).to_be_visible()

            page.get_by_label('Results mode', exact=True).select_option('stream')
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_text('2000 rows received.', exact=False)).to_be_visible()
            expect(page.get_by_text('Preview is limited to 1,000 rows and 8 MiB of encoded row data. Earlier rows were discarded.', exact=True)).to_be_visible()
            expect(page.get_by_role('button', name='Run Query', exact=True)).to_be_enabled()
            assert page.locator('tbody tr').count() == 50

            page.get_by_label('Results mode', exact=True).select_option('cursor')
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_role('button', name='Next server page', exact=True)).to_be_enabled()
            page.get_by_role('button', name='Next server page', exact=True).click()
            expect(page.get_by_text('150 rows received.', exact=False)).to_be_visible()
            expect(page.get_by_role('button', name='Next server page', exact=True)).to_have_count(0)
            expect(page.locator('tbody tr').first).to_contain_text('100')
            page.get_by_role('button', name='Run Query', exact=True).click()
            with page.expect_response(lambda response: response.url.endswith('/query/cursors/browser-test') and response.request.method == 'DELETE'):
                page.get_by_role('button', name='Close cursor', exact=True).click()
            expect(page.get_by_text('Stopped.', exact=False)).to_be_visible()
            editor.fill('SELECT value FROM users LIMIT 1;\nSELECT value FROM users LIMIT 1;')
            page.get_by_label('Results mode', exact=True).select_option('stream')
            expect(page.get_by_role('button', name='Run Query', exact=True)).to_be_enabled()
            page.get_by_role('button', name='Run Query', exact=True).click()
            expect(page.get_by_text('3 rows received.', exact=False)).to_be_visible()
            expect(page.locator('tbody')).to_contain_text('"marker":"script-row"')
            expect(page.get_by_text('Request key:', exact=False)).to_be_visible()
            unauthorized = True
            page.get_by_role('link', name='Documents', exact=True).click()
            expect(page.get_by_role('button', name='Sign in', exact=True)).to_be_visible()
            unauthorized = False
            expires_in = 1
            login()
            expect(page.get_by_role('button', name='Sign in', exact=True)).to_be_visible()
            expires_in = 3600
            login()
            page.reload()
            expect(page.get_by_role('button', name='Sign in', exact=True)).to_be_visible()
            assert not errors, errors
            assert not csp_errors, csp_errors
            print('PASS: production routes, deferred chunks, logos, memory-only auth, 401 invalidation, reload logout,')
            print('      single document fetch, stale-request abort, editor reset, bounded pagination/JSON, XSS escaping,')
            print('      redirect rejection, non-JSON rejection, expiry logout, response cap, graph canvas and CSP.')
            print('      CodeMirror multiline/highlighting, inline diagnostics, Ctrl+Enter, streaming retention, script receipts/JSON cells, request keys and server cursor navigation/close.')
            browser.close()
    finally:
        server.should_exit = True
        thread.join(timeout=5)
        sock.close()


if __name__ == '__main__':
    main()
