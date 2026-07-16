from __future__ import annotations

from pathlib import Path


def nexoradb_package_dir() -> Path:
    return Path(__file__).resolve().parent.parent


def dashboard_static_dir() -> Path:
    return nexoradb_package_dir() / "dashboard" / "static"


def repo_root_dir() -> Path:
    return nexoradb_package_dir().parents[3]


def frontend_dir() -> Path:
    return repo_root_dir() / "frontend"


def python_project_dir() -> Path:
    return nexoradb_package_dir().parents[2]
