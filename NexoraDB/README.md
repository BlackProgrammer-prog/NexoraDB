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

The native engine is loaded automatically after installation:

```python
import nexoradb

db = nexoradb.DocEngine("./nexoradb-data")
```
