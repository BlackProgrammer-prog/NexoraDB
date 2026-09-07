"""Request and executor safety contracts; no native database is opened."""
from types import SimpleNamespace
from unittest.mock import Mock, patch

import pytest
from fastapi import HTTPException

from nexoradb_admin.query_runner import (
    _authorize_statements, _load_nexoraql_package, _validate_transaction_blocks,
    execute_query, QueryExecuteRequest,
)


def statement(name):
    return type(name, (), {})()


def test_execute_scope_does_not_grant_write_or_ddl():
    for name in ("Insert", "DropCollection", "PurgeWal", "RunJob", "Unknown"):
        with pytest.raises(HTTPException) as error:
            _authorize_statements([statement(name)], frozenset({"query:execute"}))
        assert error.value.status_code == 403


def test_read_scope_allows_select_but_not_write():
    scopes = frozenset({"query:execute", "documents:read"})
    _authorize_statements([statement("Select")], scopes)
    with pytest.raises(HTTPException):
        _authorize_statements([statement("Select"), statement("Delete")], scopes)


def test_unclosed_transaction_rejected_before_execution():
    with pytest.raises(HTTPException):
        _validate_transaction_blocks([statement("BeginTx"), statement("Insert")])


def test_http_preflights_all_permissions_before_mutation():
    engine = Mock()
    with pytest.raises(HTTPException) as error:
        execute_query(engine=engine, graph_manager=None,
                      payload=QueryExecuteRequest(query="CREATE COLLECTION x; DROP COLLECTION y;"),
                      granted_scopes=frozenset({"query:execute", "documents:read"}))
    assert error.value.status_code == 403
    assert not engine.mock_calls


def test_failed_statement_rolls_back_and_stops_script():
    ql = _load_nexoraql_package()
    engine = Mock()
    engine.rollback_transaction.return_value = SimpleNamespace(success=True)
    executor = ql.Executor(engine)
    tx = object()
    executor._tx = tx
    executor.execute = Mock(return_value={"success": False, "error": "conflict"})
    assert len(executor.execute_statements([object(), object()])) == 1
    engine.rollback_transaction.assert_called_once_with(tx)
    assert executor._tx is None


def test_point_update_and_delete_use_active_transaction():
    ql = _load_nexoraql_package()
    semantic = __import__(ql.__name__ + ".semantic", fromlist=["build_update_spec"])
    engine = Mock()
    engine.update_by_id_tx.return_value = SimpleNamespace(success=True, data="1", error_msg="")
    engine.delete_by_id_tx.return_value = SimpleNamespace(success=True, data="1", error_msg="")
    executor = ql.Executor(engine)
    tx = object()
    executor._tx = tx
    with patch.object(semantic, "build_update_spec", return_value="spec"):
        executor.execute(ql.parse("UPDATE users SET age=2 WHERE _id='u1';")[0])
    executor.execute(ql.parse("DELETE FROM users WHERE _id='u1';")[0])
    engine.update_by_id_tx.assert_called_once_with(tx, "users", "u1", "spec")
    engine.delete_by_id_tx.assert_called_once_with(tx, "users", "u1")
    engine.update_by_id.assert_not_called()
    engine.delete_by_id.assert_not_called()


def test_transaction_scan_is_explicitly_rejected():
    ql = _load_nexoraql_package()
    engine = Mock()
    executor = ql.Executor(engine)
    executor._tx = object()
    with pytest.raises(ql.NexoraQLUnsupportedError):
        executor.execute(ql.parse("SELECT * FROM users;")[0])
    engine.find_many.assert_not_called()


def test_graph_transaction_is_rejected_and_rolled_back_without_writes():
    ql = _load_nexoraql_package()
    engine = Mock()
    engine.rollback_transaction.return_value = SimpleNamespace(success=True)
    executor = ql.Executor(engine, Mock())
    executor._tx = object()
    with pytest.raises(ql.NexoraQLUnsupportedError):
        executor.execute_text("INSERT INTO users VALUES ('{\"_id\":\"u1\"}');")
    engine.insert_one.assert_not_called()
    engine.insert_one_tx.assert_not_called()
    engine.rollback_transaction.assert_called_once()


def test_sql_comparison_aliases_and_render_are_preserved():
    ql = _load_nexoraql_package()
    assert ql.parse("SELECT * FROM users WHERE age EQ 2;")[0].where.op == "EQ"
    assert ql.parse("SELECT * FROM users WHERE age NEQ 2;")[0].where.op == "NEQ"
    assert type(ql.parse("RENDER GRAPH social;")[0]).__name__ == "RenderGraph"


def test_nested_projection_preserves_shape_and_parent_selection():
    ql = _load_nexoraql_package()
    document = {"_id": "1", "address": {"city": "Tehran", "secret": "hidden"},
                "people": [{"name": "a", "secret": "hidden"}]}
    assert ql.Executor._project([document], ["address.city", "people.name"]) == [
        {"_id": "1", "address": {"city": "Tehran"}, "people": [{"name": "a"}]}]
    assert ql.Executor._project([document], ["address", "address.city"])[0]["address"] == document["address"]


def test_parameters_are_typed_values_not_query_text():
    ql = _load_nexoraql_package()
    malicious = "'; DROP COLLECTION users; --"
    statements = ql.parse("SELECT * FROM users WHERE name=$name LIMIT $n;",
                          {"name": malicious, "n": 3})
    assert len(statements) == 1
    assert statements[0].where.value == malicious
    assert statements[0].limit == 3
    with pytest.raises(ql.NexoraQLParseError):
        ql.parse("SELECT * FROM $collection;", {"collection": "users"})


def test_object_array_literals_and_document_parameters():
    import json
    ql = _load_nexoraql_package()
    stmt = ql.parse('INSERT INTO users VALUES ($doc);',
                    {"doc": {"_id": "1", "name": "سلام", "nested": [1, None, {"x": True}]}})[0]
    assert json.loads(stmt.json_doc)["nested"] == [1, None, {"x": True}]
    stmt = ql.parse('UPDATE users SET data={"x": [1, null, true]} WHERE _id="1";')[0]
    assert stmt.ops[0].value == {"x": [1, None, True]}
    assert ql.parse('UPDATE users SET data=[] WHERE _id="1";')[0].ops[0].value == []


@pytest.mark.parametrize("parameters", [{}, {"n": True}, {"n": -1}, {"n": 2.5},
                                        {"n": 1 << 64}, {"n": float("nan")}])
def test_invalid_or_missing_limit_parameter_rejected(parameters):
    ql = _load_nexoraql_package()
    with pytest.raises(ql.NexoraQLParseError):
        ql.parse("SELECT * FROM users LIMIT $n;", parameters)


def test_parse_one_refuses_to_drop_extra_statements():
    ql = _load_nexoraql_package()
    with pytest.raises(ql.NexoraQLParseError):
        ql.parse_one("SHOW COLLECTIONS; DROP COLLECTION users;")


def test_qualified_join_predicate_and_nested_join_fields():
    ql = _load_nexoraql_package()
    stmt = ql.parse("SELECT posts.title FROM posts LOOKUP JOIN users "
                    "ON posts.meta.author = users.profile.name WHERE posts.likes > 10;")[0]
    assert stmt.where.field == "likes"
    assert stmt.projection == ["title"]
    assert stmt.joins == [("users", "meta.author", "profile.name")]


def test_point_select_skip_and_io_error_are_not_hidden():
    ql = _load_nexoraql_package()
    engine = Mock()
    executor = ql.Executor(engine)
    engine.find_by_id.return_value = SimpleNamespace(success=True, data='{"_id":"1"}', error_msg="")
    assert executor.execute_text('SELECT * FROM users WHERE _id="1" SKIP 1;')[0]["count"] == 0
    engine.find_by_id.return_value = SimpleNamespace(success=False, data="", error_msg="I/O failure", error_code="io_error")
    assert executor.execute_text('SELECT * FROM users WHERE _id="1";')[0]["success"] is False
    engine.find_by_id.return_value.error_code = "not_found"
    assert executor.execute_text('SELECT * FROM users WHERE _id="1";')[0]["count"] == 0


def test_driver_rejects_identifier_injection_before_network():
    from nexoradb.api.driver import NexoraDBClient
    client = NexoraDBClient(url="http://localhost", token="test")
    client._request = Mock()
    for operation in (client.count, client.create_collection, client.find):
        with pytest.raises(ValueError):
            operation("users; DROP COLLECTION users")
    client._request.assert_not_called()


def test_graph_startup_failure_is_not_published():
    from nexoradb_admin import native
    manager = Mock()
    manager.startup.return_value = False
    module = SimpleNamespace(GRAPH_ENABLED=True, GraphManager=Mock(return_value=manager))
    with patch.object(native, "load_native_module", return_value=module):
        with pytest.raises(RuntimeError, match="not ready"):
            native.create_graph_manager(SimpleNamespace(graph_dir="unused"), Mock())
    manager.shutdown.assert_called_once()


def test_defaults_distinguish_explicit_null_from_no_default():
    ql = _load_nexoraql_package()
    fields = ql.parse('CREATE COLLECTION items (a STRING, b OBJECT DEFAULT null, c INT32 DEFAULT 18);')[0].fields
    assert fields[0].has_default is False
    assert fields[1].has_default is True and fields[1].default is None
    assert fields[2].has_default is True and fields[2].default == 18


def test_duplicate_literals_and_excessive_nesting_are_rejected():
    ql = _load_nexoraql_package()
    with pytest.raises(ql.NexoraQLParseError):
        ql.parse('UPDATE items SET value={"a":1,"a":2} WHERE _id="1";')
    with pytest.raises(ql.NexoraQLParseError):
        ql.parse('SELECT * FROM items WHERE ' + '(' * 65 + 'true' + ')' * 65 + ';')
    # Delimiters in a literal are data, not AST nesting.
    stmt = ql.parse('SELECT * FROM items WHERE text=$text;', {"text": '(' * 100})[0]
    assert stmt.where.value == '(' * 100


def test_driver_sends_parameters_separately_and_reports_statement_errors():
    from nexoradb.api.driver import NexoraDBClient, NexoraDBError
    client = NexoraDBClient(url="http://localhost", token="test")
    client._request = Mock(return_value={"columns": [], "rows": [], "raw": {"statements": [{"success": True}]}})
    client.insert_one("items", {"text": "';DROP COLLECTION items;--"})
    body = client._request.call_args.args[2]
    assert body["query"] == "INSERT INTO items VALUES ($document);"
    assert body["parameters"]["document"]["text"].startswith("';")
    client._request.return_value = {"raw": {"statements": [{"success": False, "error": "conflict"}]}}
    with pytest.raises(NexoraDBError, match="conflict"):
        client.execute("SELECT * FROM items;")


def test_sql_graph_edge_syntax_and_parameters_share_the_existing_ast():
    ql = _load_nexoraql_package()
    sql = ql.parse('EDGE EXISTS FROM User($a) TO User($b) TYPE FOLLOWS;',
                   {"a": "u1", "b": "u2"})[0]
    legacy = ql.parse('EDGE EXISTS User("u1") -[FOLLOWS]-> User("u2");')[0]
    assert sql == legacy
    traversal = ql.parse('TRAVERSE User($id) OUT FOLLOWS DEPTH $depth LIMIT $limit;',
                         {"id": "u1", "depth": 2, "limit": 20})[0]
    assert (traversal.node_id, traversal.depth, traversal.limit) == ("u1", 2, 20)
    assert ql.parse('SELECT * FROM users WHERE name LIKE $pattern;', {"pattern": "^ali"})[0].where.value == "^ali"
