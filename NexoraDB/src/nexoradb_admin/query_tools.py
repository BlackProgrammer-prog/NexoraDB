"""Non-executing diagnostics and program preflight for bounded result modes."""
import importlib

from fastapi import HTTPException

from .query_runner import (_load_nexoraql_package, _normalize_query,
                           _raise_query_error, _validate_transaction_blocks, query_diagnostic)


def validate_query(payload):
    parser = _load_nexoraql_package()
    try:
        statements = parser.parse(_normalize_query(payload.query), payload.parameters)
        _validate_transaction_blocks(statements)
        semantic = importlib.import_module(parser.__name__ + '.semantic')
        validator = semantic.Validator()
        for statement in statements:
            validator.validate(statement)
        prepare_program(payload)
        return {'valid': True, 'diagnostics': [], 'statementCount': len(statements),
                'cursorSupported': True,
                'executionStrategy': 'prepared' if any(type(s).__name__ not in READ_ONLY for s in statements) else 'incremental',
                'requiresIdempotencyKey': any(type(s).__name__ not in READ_ONLY for s in statements)}
    except (parser.NexoraQLError, HTTPException) as exc:
        if isinstance(exc, HTTPException):
            message = exc.detail.get('message', 'Invalid query') if isinstance(exc.detail, dict) else str(exc.detail)
            diagnostic = dict(message=message, line=None, column=None)
        else:
            diagnostic = query_diagnostic(exc, payload.query)
        return {'valid': False, 'diagnostics': [dict(diagnostic, severity='error')],
                'statementCount': 0, 'cursorSupported': False}


READ_ONLY = frozenset({'Select', 'Count', 'ExistsStmt', 'ShowCollections', 'CollectionExists',
    'DescribeCollection', 'ShowIndexes', 'ShowForeignKeys', 'ShowGraphs', 'DescribeGraph',
    'GraphStatus', 'GraphStats', 'Traverse', 'GetNode', 'EdgeExists', 'SystemStatus', 'SystemInfo',
    'ShowPerformance', 'ShowIndexAdvice', 'ShowSecurityUsers', 'ShowSecurityRoles'})


def validate_joins(statement):
    joins = getattr(statement, 'joins', [])
    if len(joins) > 8 or len({j[0] for j in joins}) != len(joins):
        raise HTTPException(422, 'Use at most eight LOOKUP JOIN targets with distinct collection names')


def prepare_program(payload):
    parser = _load_nexoraql_package()
    try:
        statements = parser.parse(_normalize_query(payload.query), payload.parameters)
        if len(statements) > 256:
            raise HTTPException(422, 'At most 256 statements per program')
        _validate_transaction_blocks(statements)
        semantic = importlib.import_module(parser.__name__ + '.semantic')
        active = False
        for statement in statements:
            name = type(statement).__name__
            semantic.Validator().validate(statement)
            validate_joins(statement)
            if not hasattr(semantic.Executor, '_x_' + name):
                raise HTTPException(422, 'Unsupported statement: ' + name)
            if active:
                if name not in {'Select', 'Insert', 'InsertBatch', 'Update', 'Delete', 'CommitTx', 'RollbackTx'}:
                    raise HTTPException(422, name + ' is unsupported inside transactions')
                if name == 'Select' and (statement.joins or semantic._extract_id_eq(statement.where) is None):
                    raise HTTPException(422, 'Transactional SELECT requires an exact _id predicate')
                if name in {'Update', 'Delete'} and (semantic._extract_id_eq(statement.where) is None or getattr(statement, 'many', False)):
                    raise HTTPException(422, 'Transactional UPDATE/DELETE requires an exact _id predicate')
            if name == 'BeginTx': active = True
            if name in {'CommitTx', 'RollbackTx'}: active = False
            if getattr(statement, 'skip', 0) > 1_000_000:
                raise HTTPException(422, 'SKIP exceeds 1000000; prefer a filtered query')
        return statements, semantic
    except Exception as exc:
        _raise_query_error(exc, parser, payload.query)
