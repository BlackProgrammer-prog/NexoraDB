"""Incremental read programs and disk-bounded, replay-protected mutation results.

Each SELECT owns a snapshot, including its lookup targets. Separate statements
observe separate snapshots (read committed), not a fictitious script-wide one.
Mutation programs finish before delivery; no transaction crosses HTTP pages.
"""
import json
import os
import secrets
import tempfile
import time
from contextlib import nullcontext
from pathlib import Path
from threading import RLock

from fastapi import HTTPException

from .execution_control import current_execution
from .idempotency import RequestJournal
from .query_runner import _rows_from_single_result, _raise_query_error, _load_nexoraql_package
from .query_tools import READ_ONLY


def validate_graph_program(statements, graph_manager, engine=None):
    if graph_manager is None or getattr(engine, "native_projection_on_commit", False) is True: return
    active = False
    for statement in statements:
        name = type(statement).__name__
        if active and name in {'Insert', 'InsertBatch', 'Update', 'Delete'}:
            raise HTTPException(422, 'Transactional graph projection is unsupported; no statements were executed')
        if name == 'BeginTx': active = True
        if name in {'CommitTx', 'RollbackTx'}: active = False


class ProgramStream:
    def __init__(self, engine, statements, semantic, graph_manager=None, authorize=lambda: None):
        validate_graph_program(statements, graph_manager, engine)
        self.engine, self.statements, self.semantic = engine, statements, semantic
        self.authorize = authorize
        self.executor = semantic.Executor(engine, graph_manager)
        token = current_execution.get()
        self.executor.completed_statements = token.completed if token else []
        def check():
            active = current_execution.get()
            if active: active.check()
        self.executor.execution_check = check
        self.native = None
        self.errors = []
        scope_factory = getattr(engine, 'execution_scope', None)
        self.scope = scope_factory(120000) if scope_factory else None
        if token and self.scope is not None: token.attach(self.scope)
        self.iterator = self._rows()
        self.raw_mode = False
        self.raw_done = False
        self.raw_remaining = (statements[0].limit or None) if type(statements[0]).__name__ == 'Select' else None

    def __next__(self):
        if self.raw_mode: raise RuntimeError('Cannot mix raw batches and decoded iteration')
        active = current_execution.get()
        if active and self.scope is not None: active.attach(self.scope)
        try:
            with self.scope if self.scope is not None else nullcontext():
                return next(self.iterator)
        except StopIteration:
            raise
        except RuntimeError as exc:
            raise HTTPException(400, 'Query execution failed; reconcile the request key before retrying writes') from exc
        except Exception as exc:
            _raise_query_error(exc, _load_nexoraql_package())

    @property
    def next_ndjson_batch(self):
        statement = self.statements[0]
        if (len(self.statements) != 1 or type(statement).__name__ != 'Select'
                or statement.projection is not None or statement.skip or self.scope is None):
            raise AttributeError('Decoded program iteration required')
        return self._raw_batch

    def _raw_batch(self, size):
        self.raw_mode = True
        if self.raw_done: return b'', 0
        active = current_execution.get()
        if active:
            active.check()
            if self.scope is not None: active.attach(self.scope)
        self.authorize()
        statement = self.statements[0]
        with self.scope if self.scope is not None else nullcontext():
            if self.native is None:
                kwargs = dict(page_size=128, max_page_bytes=1024*1024)
                condition = self.semantic.build_condition(statement.where)
                self.native = (self.engine.iter_join_documents(statement.collection, statement.joins, condition, **kwargs)
                               if statement.joins else self.engine.iter_documents(statement.collection, condition, **kwargs))
            chunk, count = self.native.next_ndjson_batch(min(size, self.raw_remaining) if self.raw_remaining is not None else size)
            if self.raw_remaining is not None: self.raw_remaining -= count
            if not count or self.raw_remaining == 0:
                self.raw_done = True
                self.native.close()
                self.native = None
            return chunk, count

    def _rows(self):
        multiple = len(self.statements) > 1
        try:
            for index, statement in enumerate(self.statements, 1):
                self.executor.execution_check()
                self.authorize()
                name, count = type(statement).__name__, 0
                if name == 'Select' and self.executor._tx is None:
                    kwargs = dict(page_size=128, max_page_bytes=1024 * 1024)
                    condition = self.semantic.build_condition(statement.where)
                    if statement.joins:
                        self.native = self.engine.iter_join_documents(statement.collection, statement.joins, condition, **kwargs)
                    else:
                        self.native = self.engine.iter_documents(statement.collection, condition, **kwargs)
                    project = self.semantic.Executor._compile_projection(statement.projection)
                    skip, remaining, scanned = statement.skip, statement.limit or None, 0
                    try:
                        while remaining is None or remaining > 0:
                            self.executor.execution_check()
                            if scanned % 128 == 0: self.authorize()
                            row = next(self.native, None)
                            if row is None: break
                            scanned += 1
                            if skip:
                                skip -= 1
                                continue
                            row = project(row)
                            count += 1
                            yield {'statement': index, 'statementType': name, 'row': row} if multiple else row
                            if remaining is not None: remaining -= 1
                    finally:
                        self.native.close()
                        self.native = None
                    result = {'success': True, 'count': count}
                    self.executor.completed_statements.append({'statement': index, 'type': name, 'success': True,
                                                                'outcome': 'completed'})
                else:
                    result = self.executor.execute_statements([statement])[0]
                    if result.get('success') is False:
                        self.errors.append(str(result.get('error') or 'Statement failed'))
                    for row in _rows_from_single_result(result):
                        count += 1
                        yield {'statement': index, 'statementType': name, 'row': row} if multiple else row
                if multiple:
                    yield {'statement': index, 'statementType': name, 'receipt': True,
                           'success': result.get('success', True), 'rowCount': count,
                           'error': result.get('error') or None}
                if result.get('success') is False:
                    break
            if any(type(s).__name__ not in READ_ONLY for s in self.statements):
                yield {'executionReceipt': True, 'success': all(r.get('success') is not False for r in self.executor.completed_statements),
                       'statements': self.executor.completed_statements,
                       'outcome': 'completed_prefix_preserved; transactions_committed_or_rolled_back'}
        finally:
            self.executor.close_execution()

    def close(self):
        try:
            self.iterator.close()
        finally:
            if self.native is not None:
                self.native.close()
                self.native = None
            self.executor.close_execution()


def needs_graph_manager(statements):
    return any(type(s).__name__ not in READ_ONLY or 'Graph' in type(s).__name__ or type(s).__name__ in
               {'Traverse', 'GetNode', 'EdgeExists'} for s in statements)


class SpoolReader:
    def __init__(self, path, errors, on_close):
        self.file = open(path, 'rb')
        self.errors = errors
        self.on_close = on_close
        self.closed = False

    def __next__(self):
        line = self.file.readline(2 * 1024 * 1024 + 1)
        if not line: raise StopIteration
        if len(line) > 2 * 1024 * 1024: raise RuntimeError('Spool record exceeds budget')
        return json.loads(line)

    def close(self):
        if not self.closed:
            self.closed = True
            try: self.file.close()
            finally: self.on_close()


class ProgramResults:
    """Private process-local spools; durable journal never replays expired writes.

    Journal is deliberately independent of RocksDB. Ambiguous failures require
    reconciliation, rather than an unsafe claim of crash-atomic exactly-once work.
    """
    def __init__(self, journal_path, *, total_bytes=256*1024*1024, program_bytes=64*1024*1024, lifetime=120):
        self.directory = tempfile.TemporaryDirectory(prefix='nexoradb-results-')
        os.chmod(self.directory.name, 0o700)
        self.journal = RequestJournal(journal_path)
        self.total_bytes, self.program_bytes, self.lifetime = total_bytes, program_bytes, lifetime
        self.used = 0
        self.entries = {}
        self.lock = RLock()

    def reap(self):
        with self.lock:
            for key, (path, size, created, readers) in list(self.entries.items()):
                if not readers and time.monotonic() - created >= self.lifetime:
                    path.unlink(missing_ok=True)
                    self.used -= size
                    del self.entries[key]

    def prepare(self, owner, key, payload, factory):
        if not key:
            raise HTTPException(428, 'Idempotency-Key is required for programs with side effects')
        self.reap()
        def execute():
            identifier = secrets.token_hex(24)
            path = Path(self.directory.name) / identifier
            size = 0
            with self.lock:
                if self.used + self.program_bytes > self.total_bytes:
                    raise HTTPException(507, 'Result spool quota exhausted; program was not started')
                self.used += self.program_bytes
            stream = None
            try:
                stream = factory()
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, 'O_NOFOLLOW', 0), 0o600)
                with os.fdopen(fd, 'wb') as output:
                    while True:
                        row = next(stream, None)
                        if row is None: break
                        line = (json.dumps(row, ensure_ascii=False, allow_nan=False, separators=(',', ':')) + '\n').encode()
                        if len(line) > 2*1024*1024 or size + len(line) > self.program_bytes:
                            raise HTTPException(413, 'Prepared results exceed budget; writes may have completed, reconcile using the request key')
                        size += len(line)
                        output.write(line)
                    output.flush()
                    os.fsync(output.fileno())
                with self.lock:
                    self.used -= self.program_bytes - size
                    self.entries[identifier] = [path, size, time.monotonic(), 0]
                return {'spool': identifier, 'receipts': stream.executor.completed_statements, 'errors': stream.errors}
            except BaseException:
                path.unlink(missing_ok=True)
                with self.lock: self.used -= self.program_bytes
                raise
            finally:
                if stream is not None: stream.close()
        result = self.journal.run(owner, key, payload, execute)
        with self.lock:
            entry = self.entries.get(result['spool'])
            if not entry or time.monotonic() - entry[2] >= self.lifetime:
                raise HTTPException(410, detail={'message': 'Results expired; the request will not execute again',
                                                'receipts': result['receipts']})
            entry[3] += 1
            def release():
                with self.lock: entry[3] -= 1
                self.reap()
            try:
                return SpoolReader(entry[0], result['errors'], release)
            except BaseException:
                entry[3] -= 1
                raise

    def shutdown(self):
        self.directory.cleanup()
