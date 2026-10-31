# NexoraDB

Current version: **0.5.0**. [Release notes](https://github.com/BlackProgrammer-prog/NexoraDB/blob/main/docs/releases/v0.5.0.md).

NexoraDB is a hybrid document and graph database engine with a Python API,
FastAPI administration service, NexoraQL parser, CLI, and bundled web dashboard.

For installation, usage, architecture, and development documentation, see the
[project repository](https://github.com/BlackProgrammer-prog/NexoraDB).

NexoraDB is licensed under the Apache License 2.0.

## Platform support

The binary wheels bundle the native C++ database engine and currently
support **Ubuntu Linux on x86_64 with CPython 3.10 only**. Windows, macOS,
other Linux distributions, ARM processors, PyPy, and other Python versions
are not supported by this release. WSL is supported through its Ubuntu Linux environment,
not as a native Windows wheel.

## Version 0.3.0 upgrade notice

This release changes native interfaces and storage/recovery behavior. Back up
documents **and** graphs while the old deployment is stopped before opening the
only copy with a newer engine. Rebuild the native module; do not mix a 0.2.x
library with 0.3.0 Python files. Legacy raw graph files require explicit source
rebuild/conversion, and graph-only mutations are not reproduced by source rebuild.
Rollback uses a pre-upgrade backup in a new directory, not an in-place downgrade.
See the repository's `docs/releases/v0.3.0.md` and `docs/cpp/PRODUCTION_RECOVERY.md`.

## Install from PyPI

Create a virtual environment and install the package:

```bash
python3.10 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
pip install NexoraDataBase
```

The package name on PyPI is `NexoraDataBase`; the name used in Python code is
`nexoradb`. Confirm that the native engine loads correctly:

```bash
python -c "import nexoradb; print(nexoradb.__version__)"
```

## Basic usage

```python
import nexoradb

db = nexoradb.DocEngine("./nexoradb-data")
db.create_collection("users")
db.insert_one("users", '{"_id":"u1","name":"Alice","age":28}')

user = db.find_by_id("users", "u1")
print(user.data)
```

Upgrade to the latest published version with:

```bash
pip install --upgrade NexoraDataBase
```
