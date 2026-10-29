"""Native engine adapters. All writes request durable acknowledgment.

PostgreSQL stores the same logical document in JSONB; this is explicitly a
document-model comparison, not a typed relational-schema comparison.
"""
import json
import re
import threading
from pathlib import Path
from contextlib import contextmanager
from .pagination import MAX_PAGE_RECORDS, DEFAULT_PAGE_BYTES, MAX_PAGE_BYTES, validate_page_size, validate_page_bytes


class NotSupported(RuntimeError):
    pass


class Adapter:
    def __init__(self, namespace='scale_lab_records'):
        if not re.fullmatch(r'scale_lab_[a-z0-9_]+', namespace):
            raise ValueError('namespace must start with scale_lab_ and contain lowercase identifiers')
        self.namespace = namespace

    def insert(self, value):
        self.batch_insert([value])

    def restore_batch(self, updates):
        for key, status in updates:
            self.update(key, status)

    def load_metrics(self):
        return {}

    def scan(self, *args):
        raise NotSupported('NOT_SUPPORTED: equivalent ordered scan not implemented')

    def read_by_field(self, *args):
        raise NotSupported('NOT_SUPPORTED: equivalent bounded field lookup not implemented')

    def aggregate(self, *args):
        raise NotSupported('NOT_SUPPORTED: equivalent aggregation not implemented')

    @contextmanager
    def worker_session(self):
        yield


def native_result(result):
    if not result.success:
        raise RuntimeError(result.error_msg)
    return result.data


def stored(value):
    return {**value, '_id': value['id']}


def logical(value):
    if value is None:
        return None
    value = dict(value)
    value.pop('_id', None)
    return value


class NexoraAdapter(Adapter):
    engine_name = 'nexoradb'
    def __init__(self, path, namespace='scale_lab_records', memory_budget_bytes=256*1024*1024, block_cache_bytes=0,
                 ingestion_mode='transactional', wal_range_sync_mb=1):
        super().__init__(namespace)
        self.path = path
        self.memory_budget_bytes = memory_budget_bytes
        self.block_cache_bytes = block_cache_bytes
        if ingestion_mode not in {'transactional','exclusive-bulk'}:
            raise ValueError('Invalid native ingestion mode')
        self.ingestion_mode,self.wal_range_sync_mb=ingestion_mode,wal_range_sync_mb

    def connect(self):
        import nexoradb as native
        self.native = native
        if (getattr(native, 'PAGE_MAX_RECORDS', None) != MAX_PAGE_RECORDS or
                getattr(native, 'PAGE_DEFAULT_BYTES', None) != DEFAULT_PAGE_BYTES or
                getattr(native, 'PAGE_MAX_BYTES', None) != MAX_PAGE_BYTES):
            raise RuntimeError('Native pagination contract is missing or incompatible; rebuild the extension')
        if not hasattr(native, 'DocumentDurability'):
            raise RuntimeError('Native extension lacks explicit durability; rebuild current source')
        if not hasattr(native.DocEngine, 'durability_report'):
            raise RuntimeError('Native durability reporting missing; rebuild extension before opening storage')
        settings = native.RocksSettings()
        settings.durability = native.DocumentDurability.Durable
        settings.memory_budget_bytes = self.memory_budget_bytes
        settings.block_cache_bytes = self.block_cache_bytes
        settings.wal_bytes_per_sync = self.wal_range_sync_mb*1024**2
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self.engine = native.DocEngine(self.path, native.TransactionSettings(), settings)
        if not hasattr(self.engine, 'durability_report'):
            raise RuntimeError('Native durability reporting missing; rebuild extension')
        effective = json.loads(self.engine.durability_report())
        if (effective.get('profile') != 'durable' or effective.get('sync_on_ack') is not True
                or effective.get('wal_enabled') is not True or effective.get('manual_wal_flush') is not False):
            raise RuntimeError('Benchmark requires effective durable WAL ACKs')

    def disconnect(self):
        if hasattr(self, 'engine'):
            native_result(self.engine.prepare_shutdown())
            del self.engine

    def prepare_schema(self):
        if self.engine.collection_exists(self.namespace):
            raise RuntimeError('Namespace exists; use a new namespace for a fresh experiment')
        native_result(self.engine.create_collection(self.namespace))

    def create_indexes(self):
        index = self.native.IndexDefinition()
        index.index_name = 'username_idx'
        index.fields = ['username']
        index.type = self.native.IndexType.SingleField
        native_result(self.engine.create_index(self.namespace, index))

    def batch_insert(self, values):
        from bson import BSON
        self.batch_insert_bson([BSON.encode(stored(v)) for v in values])

    def batch_insert_bson(self, encoded):
        if self.ingestion_mode=='exclusive-bulk':
            count=int(native_result(self.engine.insert_many_bson_ingest(self.namespace,encoded)))
        else:
            count=len(json.loads(native_result(self.engine.insert_many_bson(self.namespace,encoded))))
        if count != len(encoded):
            raise RuntimeError('Batch acknowledgment count mismatch')

    def load_metrics(self):
        metrics = self.engine.rocks_metrics()
        result = {name: getattr(metrics, name) for name in (
            'logical_write_bytes', 'wal_write_bytes', 'flush_write_bytes',
            'compaction_write_bytes', 'pending_compaction_bytes', 'stall_micros',
            'l0_files', 'memtable_usage', 'memtable_budget', 'cache_usage',
            'cache_capacity', 'running_compactions', 'running_flushes', 'write_stopped')}
        result['write_path'] = json.loads(self.engine.operation_metrics())['write_path']
        result['memory'] = json.loads(self.engine.memory_report())
        return result

    def read_by_id(self, key):
        result = self.engine.find_by_id(self.namespace, key)
        if not result.success and getattr(result, 'error_code', '') == 'not_found':
            return None
        return logical(json.loads(native_result(result)))

    def update(self, key, status):
        spec = self.native.UpdateSpec()
        spec.set('status', status)
        native_result(self.engine.update_by_id(self.namespace, key, spec))

    def delete(self, key):
        return int(native_result(self.engine.delete_by_id(self.namespace, key)))

    def restore_batch(self, updates):
        tx = self.engine.begin_transaction()
        if tx is None or not tx.is_valid():
            raise RuntimeError('Could not start restoration transaction')
        try:
            for key, status in updates:
                spec = self.native.UpdateSpec()
                spec.set('status', status)
                native_result(self.engine.update_by_id_tx(tx, self.namespace, key, spec))
            native_result(self.engine.commit_transaction(tx))
        except BaseException:
            if tx.is_valid():
                native_result(self.engine.rollback_transaction(tx))
            raise

    def count(self):
        return int(native_result(self.engine.count(self.namespace)))

    def iter_records(self, batch_size=MAX_PAGE_RECORDS, max_page_bytes=DEFAULT_PAGE_BYTES):
        validate_page_size(batch_size)
        validate_page_bytes(max_page_bytes)
        stream = self.engine.iter_documents(self.namespace, page_size=batch_size, max_page_bytes=max_page_bytes)
        try:
            for value in stream:
                yield logical(value)
        finally:
            stream.close()

    def read_by_field(self, field, value):
        if field != 'username':
            raise NotSupported('NOT_SUPPORTED: only username lookup is certified')
        condition = self.native.Condition.leaf(field, self.native.Op.EQ, value)
        return [logical(v) for v in json.loads(native_result(
            self.engine.find_many(self.namespace, condition, 2, 0)))]

    def metadata(self):
        import os
        return {'deployment': 'embedded', 'durability': 'RocksDB WAL sync=true',
                'effective_durability': json.loads(self.engine.durability_report()),
                'native_build': self.native.build_info(),
                'native_binary': str(Path(self.native.__file__).resolve()),
                'resource_roots': [os.getpid()], 'resource_attribution': 'engine_and_generator_combined',
                'settings': {'memory_budget_bytes': self.memory_budget_bytes,
                             'block_cache_bytes': self.engine.rocks_metrics().cache_capacity,
                             'ingestion_mode': self.ingestion_mode, 'wal_range_sync_mb': self.wal_range_sync_mb},
                'ingestion_api': ('insert_many_bson_ingest (exclusive, validated, atomic, durable, count ACK)'
                                  if self.ingestion_mode=='exclusive-bulk' else 'insert_many_bson (validated, atomic, durable)'),
                'query_tuning': json.loads(self.engine.query_tuning()),
                'indexes': [{'name': i.index_name, 'fields': list(i.fields), 'type': str(i.type),
                             'state': str(i.state)} for i in self.engine.get_indexes(self.namespace)],
                'operation_timeout': 'NOT_SUPPORTED by native per-operation API'}

    def lookup_plan(self):
        condition = self.native.Condition.leaf('username', self.native.Op.EQ, 'user_0')
        plan = json.loads(native_result(self.engine.explain_plan(self.namespace, condition)))
        return {'plan': plan, 'uses_secondary_index': plan.get('index') == 'username_idx'
                and plan.get('scan_type') != 'full_scan'}

    def cleanup(self):
        native_result(self.engine.drop_collection(self.namespace))


class PostgreSQLAdapter(Adapter):
    engine_name = 'postgresql'
    def __init__(self, dsn, namespace='scale_lab_records'):
        super().__init__(namespace)
        self.dsn = dsn
        self.local = threading.local()
        self.connections = []
        self.lock = threading.Lock()

    def connect(self):
        self.connection()

    def connection(self):
        if not hasattr(self.local, 'connection'):
            import psycopg
            connection = psycopg.connect(self.dsn, autocommit=True, connect_timeout=10)
            connection.execute("SET synchronous_commit = on")
            connection.execute("SET statement_timeout = 0")
            if connection.execute('SHOW fsync').fetchone()[0] != 'on':
                connection.close()
                raise RuntimeError('PostgreSQL fsync must be on')
            self.local.connection = connection
            with self.lock:
                self.connections.append(connection)
        return self.local.connection

    def disconnect(self):
        for connection in self.connections:
            connection.close()
        self.connections.clear()

    @contextmanager
    def worker_session(self):
        connection = self.connection()
        connection.execute('SET statement_timeout = 30000')
        try:
            yield
        finally:
            connection.close()
            del self.local.connection
            with self.lock:
                self.connections.remove(connection)

    def prepare_schema(self):
        self.connection().execute(f'CREATE TABLE {self.namespace} (id text PRIMARY KEY, doc jsonb NOT NULL)')

    def create_indexes(self):
        self.connection().execute(f'CREATE INDEX ON {self.namespace} ((doc->>\'username\'))')

    def batch_insert(self, values):
        from psycopg.types.json import Jsonb
        connection = self.connection()
        with connection.transaction(), connection.cursor() as cursor:
            cursor.executemany(f'INSERT INTO {self.namespace} VALUES (%s,%s)',
                               [(v['id'], Jsonb(v)) for v in values])

    def read_by_id(self, key):
        row = self.connection().execute(f'SELECT doc FROM {self.namespace} WHERE id=%s', (key,)).fetchone()
        return row[0] if row else None

    def update(self, key, status):
        cursor = self.connection().execute(
            f"UPDATE {self.namespace} SET doc=jsonb_set(doc,'{{status}}',to_jsonb(%s::text)) WHERE id=%s",
            (status, key))
        if cursor.rowcount != 1:
            raise RuntimeError('Update did not match exactly one record')

    def delete(self, key):
        return self.connection().execute(f'DELETE FROM {self.namespace} WHERE id=%s', (key,)).rowcount

    def restore_batch(self, updates):
        with self.connection().transaction():
            for key, status in updates:
                self.update(key, status)

    def count(self):
        return self.connection().execute(f'SELECT count(*) FROM {self.namespace}').fetchone()[0]

    def iter_records(self, batch_size=1000):
        validate_page_size(batch_size)
        import psycopg
        with psycopg.connect(self.dsn, connect_timeout=10) as connection:
            with connection.cursor(name='scale_lab_validation') as cursor:
                cursor.itersize = batch_size
                cursor.execute(f'SELECT doc FROM {self.namespace}')
                for row in cursor:
                    yield row[0]

    def read_by_field(self, field, value):
        if field != 'username':
            raise NotSupported('NOT_SUPPORTED: only username lookup is certified')
        return [r[0] for r in self.connection().execute(
            f"SELECT doc FROM {self.namespace} WHERE doc->>'username'=%s LIMIT 2", (value,))]

    def metadata(self):
        import os
        connection = self.connection()
        settings = {name: connection.execute('SHOW ' + name).fetchone()[0] for name in
                    ('server_version', 'fsync', 'synchronous_commit', 'full_page_writes',
                     'shared_buffers', 'work_mem', 'max_connections', 'listen_addresses', 'port')}
        for name in ('maintenance_work_mem', 'autovacuum_work_mem', 'max_wal_size', 'min_wal_size',
                     'checkpoint_timeout', 'checkpoint_completion_target', 'wal_buffers',
                     'max_parallel_workers_per_gather', 'jit', 'effective_cache_size'):
            settings[name] = connection.execute('SHOW ' + name).fetchone()[0]
        backend_pid = connection.execute('SELECT pg_backend_pid()').fetchone()[0]
        # The local backend's parent is the postmaster; remote hosts are rejected
        # by the publication protocol's connection preflight.
        parent = int(Path(f'/proc/{backend_pid}/stat').read_text().rsplit(')', 1)[1].split()[1])
        from .memory_control import group_snapshot
        import psycopg
        return {'deployment': 'local TCP', 'durability': 'fsync=on, synchronous_commit=on',
                'driver_version': psycopg.__version__, 'settings': settings,
                'resource_roots': [parent], 'resource_attribution': 'server_process_tree',
                'memory_containment': group_snapshot('postgresql'),
                'indexes': [r[0] for r in connection.execute(
                    'SELECT indexdef FROM pg_indexes WHERE schemaname=current_schema() AND tablename=%s',
                    (self.namespace,))], 'generator_pid': os.getpid()}

    def optimize(self):
        self.connection().execute(f'ANALYZE {self.namespace}')

    def lookup_plan(self):
        plan = self.connection().execute(
            f"EXPLAIN (FORMAT JSON) SELECT doc FROM {self.namespace} WHERE doc->>'username'=%s LIMIT 2",
            ('user_0',)).fetchone()[0]
        return {'plan': plan, 'uses_secondary_index': 'Index Name' in json.dumps(plan)}

    def cleanup(self):
        self.connection().execute(f'DROP TABLE {self.namespace}')


class MongoDBAdapter(Adapter):
    engine_name = 'mongodb'
    def __init__(self, uri, namespace='scale_lab_records'):
        super().__init__(namespace)
        self.uri = uri

    def connect(self):
        from pymongo import MongoClient
        from pymongo.write_concern import WriteConcern
        self.client = MongoClient(self.uri, serverSelectionTimeoutMS=10000,
                                  connectTimeoutMS=10000, socketTimeoutMS=30000,
                                  maxPoolSize=600, retryWrites=False, retryReads=False)
        self.client.admin.command('ping')
        self.database = self.client.get_default_database(default='scale_lab')
        self.collection = self.database.get_collection(
            self.namespace, write_concern=WriteConcern(w=1, j=True))

    def disconnect(self):
        if hasattr(self, 'client'):
            self.client.close()

    def prepare_schema(self):
        self.database.create_collection(self.namespace)

    def create_indexes(self):
        self.collection.create_index('username', name='username_idx')

    def batch_insert(self, values):
        result = self.collection.insert_many([stored(v) for v in values], ordered=True)
        if len(result.inserted_ids) != len(values):
            raise RuntimeError('Batch acknowledgment count mismatch')

    def read_by_id(self, key):
        return logical(self.collection.find_one({'_id': key}))

    def update(self, key, status):
        if self.collection.update_one({'_id': key}, {'$set': {'status': status}}).matched_count != 1:
            raise RuntimeError('Update did not match exactly one record')

    def delete(self, key):
        return self.collection.delete_one({'_id': key}).deleted_count

    def restore_batch(self, updates):
        from pymongo import UpdateOne
        result = self.collection.bulk_write([UpdateOne({'_id': key}, {'$set': {'status': status}})
                                             for key, status in updates], ordered=True)
        if result.matched_count != len(updates):
            raise RuntimeError('Restoration did not match every requested document')

    def count(self):
        return self.collection.count_documents({})

    def iter_records(self, batch_size=1000):
        validate_page_size(batch_size)
        with self.collection.find({}, batch_size=batch_size, no_cursor_timeout=False) as cursor:
            for value in cursor:
                yield logical(value)

    def read_by_field(self, field, value):
        if field != 'username':
            raise NotSupported('NOT_SUPPORTED: only username lookup is certified')
        return [logical(v) for v in self.collection.find({field: value}).limit(2)]

    def metadata(self):
        import pymongo
        roots = []
        for path in Path('/proc').glob('[0-9]*/comm'):
            try:
                if path.read_text().strip() == 'mongod':
                    roots.append(int(path.parent.name))
            except (FileNotFoundError, PermissionError):
                continue
        if len(roots) != 1:
            raise RuntimeError('Publication profiling requires exactly one local mongod process')
        return {'deployment': 'local TCP', 'durability': 'standalone w=1,j=true; no replication',
                'driver_version': pymongo.version, 'server_version': self.client.server_info()['version'],
                'resource_roots': roots, 'resource_attribution': 'server_process_tree',
                'indexes': list(self.collection.list_indexes()),
                'settings': {'maxPoolSize': 600, 'retryReads': False, 'retryWrites': False,
                             'server_config_path': '/etc/mongod.conf'}}

    def lookup_plan(self):
        plan = self.collection.find({'username': 'user_0'}).limit(2).explain()['queryPlanner']['winningPlan']
        return {'plan': plan, 'uses_secondary_index': 'username_idx' in json.dumps(plan)
                and 'COLLSCAN' not in json.dumps(plan)}

    def cleanup(self):
        self.collection.drop()
