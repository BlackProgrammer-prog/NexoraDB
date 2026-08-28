# NexoraDB

NexoraDB is a hybrid document and graph database engine with a Python API,
FastAPI administration service, NexoraQL parser, CLI, and bundled web dashboard.

For installation, usage, architecture, and development documentation, see the
[project repository](https://github.com/BlackProgrammer-prog/NexoraDB).

NexoraDB is licensed under the Apache License 2.0.

## Platform support

The `0.1.x` binary wheels bundle the native C++ database engine and currently
support **Ubuntu Linux on x86_64 with CPython 3.10 only**. Windows, macOS,
other Linux distributions, ARM processors, PyPy, and other Python versions
are not supported by this initial release.

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
