"""Transport adapters only. Identities, sessions and decisions belong to C++."""

import json
import time
import weakref
from contextvars import ContextVar

from fastapi import HTTPException

current_native_token = ContextVar("nexora_native_token", default="")


class NativeTokenContextMiddleware:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        header = dict(scope.get("headers", [])).get(b"authorization", b"")
        token = header[7:].decode("ascii", errors="ignore") if header.startswith(b"Bearer ") else ""
        remote = (scope.get("client") or ("", 0))[0]
        if (
            scope["type"] == "http"
            and scope.get("scheme") != "https"
            and remote not in {"127.0.0.1", "::1", "testclient"}
            and (token or scope.get("path", "").startswith("/auth/"))
        ):
            response = HTTPException(426, "Credential transport requires HTTPS")
            from starlette.responses import JSONResponse

            await JSONResponse({"message": response.detail}, status_code=426)(scope, receive, send)
            return
        handle = current_native_token.set(token)
        try:
            await self.app(scope, receive, send)
        finally:
            current_native_token.reset(handle)


class NativeIamService:
    is_native_iam = True

    def __init__(self, native, gateway, graph_directory=None):
        self.native, self.gateway = native, gateway
        self.graph = (
            SecureGraphAdapter(self, graph_directory, "")
            if graph_directory and hasattr(native, "SecureGraphGateway")
            else None
        )

    def invoke(self, operation, *args, _gateway=None):
        try:
            return getattr(self.gateway if _gateway is None else _gateway, operation)(*args)
        except self.native.AuthenticationError as error:
            raise HTTPException(401, "Session or credentials invalid") from error
        except self.native.AuthorizationError as error:
            raise HTTPException(403, str(error)) from error
        except self.native.AuthenticationRateLimited as error:
            raise HTTPException(
                429,
                "Authentication or session capacity exceeded; retry later",
                headers={"Retry-After": "60"},
            ) from error
        except ValueError as error:
            raise HTTPException(422, "Invalid security or database request") from error

    def bind(self, token=None):
        return SecureEngineAdapter(self, current_native_token.get() if token is None else token)

    def claims(self, token):
        return json.loads(self.invoke("claims", token))

    def login(self, username, password, source):
        token = self.invoke("login", username, password, source)
        claims = self.claims(token)
        return {
            "accessToken": token,
            "tokenType": "bearer",
            "expiresIn": max(0, claims["exp"] - int(time.time())),
            "user": claims["user"],
        }

    def complete_reset(self, token, password):
        return self.invoke("complete_reset", token, password)

    def prepare_shutdown(self):
        if self.graph is not None and not self.graph.prepare_shutdown():
            raise RuntimeError("Graph shutdown durability barrier failed")
        return self.gateway.prepare_shutdown()

    def is_healthy(self):
        return self.gateway.ready()


class SecurePagedStream:
    def __init__(self, engine, collection, condition, page_size, max_page_bytes, joins=()):
        self.engine, self.collection, self.condition = engine, collection, condition
        self.page_size, self.max_bytes, self.joins = page_size, max_page_bytes, joins
        self.token, self.rows, self.offset, self.ended = "", [], 0, False

    def __iter__(self):
        return self

    def __next__(self):
        # Even a buffered row must not survive revocation or idle expiration.
        self.engine.claims()
        if self.offset == len(self.rows):
            if self.ended:
                raise StopIteration
            page = self.engine._call(
                "find_page",
                self.collection,
                condition=self.condition,
                limit=self.page_size,
                page_token=self.token,
                max_bytes=self.max_bytes,
                joins=list(self.joins),
            )
            if not page.success:
                self.close()
                raise RuntimeError("Native page failed")
            self.token, self.rows, self.offset = page.continuation_token, json.loads(page.data), 0
            self.ended = not self.token
            if not self.rows:
                raise StopIteration
        row = self.rows[self.offset]
        self.offset += 1
        return row

    def next_ndjson_batch(self, size):
        rows = []
        for _ in range(min(size, 1000)):
            try:
                rows.append(next(self))
            except StopIteration:
                break
        return b"".join(
            json.dumps({"document": row}, ensure_ascii=False, separators=(",", ":")).encode()
            + b"\n"
            for row in rows
        ), len(rows)

    def close(self):
        if self.token:
            try:
                self.engine.close_page(self.token)
            except HTTPException:
                pass  # Revoked snapshots expire under the native idle lease.
        self.token, self.rows, self.ended = "", [], True


class SecureEngineAdapter:
    is_native_iam = True
    native_projection_on_commit = True

    def __init__(self, service, token):
        self.service, self.token = service, token

    def claims(self, token=None):
        return self.service.claims(self.token if token is None else token)

    def administer(self, operation, request=None):
        return json.loads(
            self.service.invoke(
                "administer",
                self.token,
                operation,
                json.dumps(request or {}, separators=(",", ":")),
            )
        )

    def _call(self, operation, collection="", **values):
        request = self.service.native.SecureDataRequest()
        request.operation, request.collection = operation, collection
        for name, value in values.items():
            if value is not None:
                setattr(request, name, value)
        return self.service.invoke("execute", self.token, request)

    def execution_scope(self, timeout_ms=30000):
        self.claims()
        return self.service.native.ExecutionScope(timeout_ms)

    def iter_documents(self, collection, condition=None, page_size=128, max_page_bytes=1024 * 1024):
        return SecurePagedStream(self, collection, condition, page_size, max_page_bytes)

    def iter_join_documents(
        self, collection, joins, condition=None, page_size=128, max_page_bytes=1024 * 1024
    ):
        return SecurePagedStream(self, collection, condition, page_size, max_page_bytes, joins)

    def is_internal_app_token_active(self, token_id):
        return self.claims()["token_id"] == token_id

    def find_by_id(self, collection, doc_id):
        return self._call("find_by_id", collection, id=doc_id)

    def find_many(self, collection, condition=None, limit=0, skip=0):
        return self._call("find_many", collection, condition=condition, limit=limit, skip=skip)

    def find_page(
        self,
        collection,
        condition=None,
        limit=100,
        continuation_token="",
        max_page_bytes=1024 * 1024,
    ):
        return self._call(
            "find_page",
            collection,
            condition=condition,
            limit=limit,
            page_token=continuation_token,
            max_bytes=max_page_bytes,
        )

    def close_page(self, continuation_token):
        return self._call("close_page", page_token=continuation_token)

    def count(self, collection, condition=None):
        return self._call("count", collection, condition=condition)

    def exists(self, collection, condition):
        return self._call("exists", collection, condition=condition)

    def create_collection(self, collection, schema=None):
        return self._call("create_collection", collection, schema=schema)

    def insert_one(self, collection, bson_json):
        return self._call("insert_one", collection, payload=bson_json)

    def insert_many(self, collection, bson_list):
        return self._call("insert_many", collection, documents=bson_list)

    def insert_many_bson(self, collection, bson_list):
        # The typed native request owns bytes until its GIL-free call completes.
        request = self.service.native.SecureDataRequest()
        request.operation, request.collection = "insert_many_bson", collection
        request.documents = bson_list
        return self.service.invoke("execute", self.token, request)

    def update_by_id(self, collection, doc_id, update_spec):
        return self._call("update_by_id", collection, id=doc_id, update=update_spec)

    def update_many(self, collection, condition, update_spec):
        return self._call("update_many", collection, condition=condition, update=update_spec)

    def delete_by_id(self, collection, doc_id):
        return self._call("delete_by_id", collection, id=doc_id)

    def delete_many(self, collection, condition):
        return self._call("delete_many", collection, condition=condition)

    def begin_transaction(self):
        return self._call("begin_transaction")

    def commit_transaction(self, transaction):
        return self._call("commit_transaction", transaction=transaction)

    def rollback_transaction(self, transaction):
        return self._call("rollback_transaction", transaction=transaction)

    def find_by_id_tx(self, transaction, collection, doc_id):
        return self._call("find_by_id_tx", collection, transaction=transaction, id=doc_id)

    def insert_one_tx(self, transaction, collection, document):
        return self._call("insert_one_tx", collection, transaction=transaction, payload=document)

    def update_by_id_tx(self, transaction, collection, doc_id, update_spec):
        return self._call(
            "update_by_id_tx", collection, transaction=transaction, id=doc_id, update=update_spec
        )

    def delete_by_id_tx(self, transaction, collection, doc_id):
        return self._call("delete_by_id_tx", collection, transaction=transaction, id=doc_id)

    def set_schema(self, collection, schema):
        return self._call("set_schema", collection, schema=schema)

    def create_index(self, collection, index_def):
        return self._call("create_index", collection, index=index_def)

    def drop_index(self, collection, index_name):
        return self._call("drop_index", collection, id=index_name)

    def add_foreign_key(self, collection, fk_def):
        return self._call("add_foreign_key", collection, foreign_key=fk_def)

    def drop_foreign_key(self, collection, fk_name):
        return self._call("drop_foreign_key", collection, id=fk_name)

    def lookup_join(
        self, from_collection, from_field, to_collection, to_field, condition=None, limit=0
    ):
        result = self._call(
            "lookup_join",
            from_collection,
            source_field=from_field,
            target_collection=to_collection,
            target_field=to_field,
            condition=condition,
            limit=limit,
        )
        return json.loads(result.data)

    def configure_query_tuning(self, read_cache_admission, smart_indexes):
        return self._call(
            "configure_query_tuning", flag=read_cache_admission, second_flag=smart_indexes
        )

    def configure_slow_queries(self, threshold_us, capacity=128):
        return self._call("configure_slow_queries", max_bytes=threshold_us, limit=capacity)

    def __getattr__(self, name):
        if name in {
            "drop_collection",
            "collection_exists",
            "get_schema",
            "get_indexes",
            "get_foreign_keys",
            "get_collection_size",
            "rebuild_indexes",
        }:
            return lambda collection: self._call(name, collection)
        if name in {
            "list_collections",
            "is_healthy",
            "rocks_metrics",
            "get_ram_usage_bytes",
            "get_disk_usage_bytes",
            "flush_storage",
            "prepare_shutdown",
            "format_versions",
        }:
            return lambda: self._call("sync_ack" if name == "flush_storage" else name)
        if name in {
            "health_report",
            "memory_report",
            "durability_report",
            "operation_metrics",
            "query_tuning",
            "drain_slow_queries",
        }:
            return lambda: self._call(name).data
        if name == "integrity_check":
            return lambda verify_indexes=True: self._call(name, flag=verify_indexes)
        if name == "create_checkpoint":
            return lambda destination: self._call(name, payload=destination)
        raise AttributeError(name)


def authenticate_token(engine, token, settings, *, admin=False):
    if getattr(engine, "is_native_iam", False):
        claims = engine.claims(token)
        if admin and (claims["role"] != "admin" or claims["service"]):
            raise HTTPException(403, "Superuser session required")
        claims["_native_token"] = token
        return claims
    from .security import decode_access_token
    from .service import require_active_admin

    payload = decode_access_token(token, settings)
    require_active_admin(engine, payload)
    return payload


class SecureGraphAdapter:
    is_native_iam = True

    def __init__(self, service, directory, token):
        self.service = weakref.proxy(service)
        self.gateway = service.native.SecureGraphGateway(service.gateway, directory, token)

    def prepare_shutdown(self):
        return self.gateway.prepare_shutdown()

    def shutdown(self):
        return self.gateway.shutdown()

    def __getattr__(self, name):
        supported = {
            "list_graphs",
            "startup",
            "create_graph",
            "drop_graph",
            "get_definition",
            "get_stats",
            "build_graph",
            "render_graph",
            "checkpoint_graph",
            "add_node_mapping",
            "add_edge_mapping",
            "create_snapshot",
            "neighbors",
            "has_edge",
            "get_wal_status",
            "on_document_inserted",
            "on_document_updated",
            "on_document_deleted",
            "run_mutual_friends",
            "run_network_stats",
            "run_most_connected",
            "run_get_friends",
            "run_are_connected",
            "run_shortest_path",
            "run_friend_suggestion",
            "run_connected_components",
            "run_community_detection",
            "run_all_distances",
            "run_betweenness_centrality",
            "run_influence_maximization",
        }
        if name not in supported:
            raise AttributeError(name)
        return lambda *args: self.service.invoke(
            "call", current_native_token.get(), name, list(args), _gateway=self.gateway
        )
