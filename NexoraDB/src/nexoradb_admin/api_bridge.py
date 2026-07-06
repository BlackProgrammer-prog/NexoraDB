from __future__ import annotations

import importlib
import importlib.util
import sys
from pathlib import Path
from types import ModuleType

_API_PACKAGE_ALIAS = "_nexoradb_external_api"


def _load_api_package() -> ModuleType:
    if _API_PACKAGE_ALIAS in sys.modules:
        return sys.modules[_API_PACKAGE_ALIAS]

    api_dir = Path(__file__).resolve().parent.parent / "nexoradb" / "api"
    init_file = api_dir / "__init__.py"
    spec = importlib.util.spec_from_file_location(
        _API_PACKAGE_ALIAS,
        init_file,
        submodule_search_locations=[str(api_dir)],
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load NexoraDB external API package from {init_file}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[_API_PACKAGE_ALIAS] = module
    spec.loader.exec_module(module)
    return module


def _api_module(name: str) -> ModuleType:
    _load_api_package()
    return importlib.import_module(f"{_API_PACKAGE_ALIAS}.{name}")


_app_module = _api_module("app")
_security_module = _api_module("security")

create_api_router = _app_module.create_api_router
AVAILABLE_APP_SCOPES = _security_module.AVAILABLE_APP_SCOPES
CreateAppTokenRequest = _security_module.CreateAppTokenRequest
CreateAppTokenResponse = _security_module.CreateAppTokenResponse
create_app_token = _security_module.create_app_token
