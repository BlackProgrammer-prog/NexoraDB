from pathlib import Path
import sys

from fastapi import FastAPI
from fastapi.testclient import TestClient
from starlette.responses import PlainTextResponse

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'NexoraDB/src'))
from nexoradb_admin.browser_security import BrowserSecurityMiddleware


def client(production=False):
    app = FastAPI()

    @app.get('/{path:path}')
    def content(path: str):
        return PlainTextResponse('ok')

    app.add_middleware(BrowserSecurityMiddleware, production=production,
                       allowed_origins=('https://console.example.com', 'http://localhost:5173',
                                        "https://bad.example/;script-src *"))
    return TestClient(app, base_url='https://testserver' if production else 'http://testserver')


def test_console_csp_and_sensitive_response_cache_policy():
    response = client().get('/admin/')
    assert response.headers['cache-control'] == 'no-store'
    csp = response.headers['content-security-policy']
    assert "script-src 'self';" in csp
    assert "frame-ancestors 'none'" in csp
    assert "base-uri 'none'" in csp
    assert 'bad.example' not in csp
    assert response.headers['x-content-type-options'] == 'nosniff'
    assert response.headers['referrer-policy'] == 'no-referrer'
    api = client().get('/auth/me')
    assert api.headers['cache-control'] == 'no-store'


def test_only_hashed_assets_are_immutable():
    c = client()
    assert 'immutable' in c.get('/admin/assets/index-1234abcd.js').headers['cache-control']
    for path in ('/admin/index.html', '/admin/brand/nexoradb-logo.svg', '/admin/assets/main.js'):
        assert c.get(path).headers['cache-control'] == 'no-store'
    assert c.get('/admin/assets/missing-1234abcd.js/nope').headers['cache-control'] == 'no-store'


def test_production_csp_and_hsts():
    response = client(production=True).get('/admin/')
    assert response.headers['strict-transport-security'] == 'max-age=31536000'
    csp = response.headers['content-security-policy']
    assert 'https://console.example.com' in csp
    assert 'wss://console.example.com' in csp
    assert 'http://localhost' not in csp
    assert 'strict-transport-security' not in client().get('/admin/').headers
