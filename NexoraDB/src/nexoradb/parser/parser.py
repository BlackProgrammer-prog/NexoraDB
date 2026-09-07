"""
nexoraql.parser
───────────────
نقطه ورود parse — تبدیل متن NexoraQL به لیست AST statements.

استفاده:
    from nexoraql.parser import parse

    statements = parse('''
        CREATE COLLECTION users;
        INSERT INTO users VALUES ('{"_id":"u1","username":"ali"}');
        SELECT * FROM users WHERE age > 18 LIMIT 10;
    ''')
    # statements = [CreateCollection(...), Insert(...), Select(...)]

وابستگی: pip install lark
"""

from __future__ import annotations

import os
import json
import math
import re
from dataclasses import fields, is_dataclass
from functools import lru_cache

from lark import Lark
from lark.exceptions import (
    UnexpectedInput,
    UnexpectedToken,
    UnexpectedCharacters,
    VisitError,
)

from .transformer import NexoraQLTransformer
from .errors import NexoraQLParseError

_GRAMMAR_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "grammar", "nexoraql.lark"
)


@lru_cache(maxsize=1)
def _get_parser() -> Lark:
    """Lark parser — یک بار ساخته و cache می‌شود."""
    with open(_GRAMMAR_PATH, encoding="utf-8") as f:
        grammar = f.read()
    return Lark(
        grammar,
        parser="earley",       # earley: انعطاف کامل برای گرامر SQL-like
        lexer="dynamic",
        propagate_positions=True,
        maybe_placeholders=False,
    )


def _validate_values(value, depth=0, budget=None):
    budget = [100_000] if budget is None else budget
    budget[0] -= 1
    if depth > 64 or budget[0] < 0:
        raise NexoraQLParseError("Query value/AST complexity limit exceeded")
    if is_dataclass(value):
        for field in fields(value):
            _validate_values(getattr(value, field.name), depth + 1, budget)
    elif isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str) or "\0" in key:
                raise NexoraQLParseError("Object keys must be strings without NUL")
            _validate_values(item, depth + 1, budget)
    elif isinstance(value, (list, tuple)):
        for item in value:
            _validate_values(item, depth + 1, budget)
    elif type(value) is int:
        if not -(1 << 63) <= value < (1 << 63):
            raise NexoraQLParseError("Integer exceeds signed 64-bit range")
    elif type(value) is float:
        if not math.isfinite(value):
            raise NexoraQLParseError("Non-finite numbers are not supported")
    elif value is not None and not isinstance(value, (str, bool)):
        raise NexoraQLParseError("Unsupported query parameter type")


_LEXICAL_PARTS = re.compile(r"--[^\r\n]*|/\*[\s\S]*?\*/|'(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"|[A-Za-z0-9_]+|[^\s]")


def _check_input(text):
    if len(text) > 200_000:
        raise NexoraQLParseError("Query exceeds 200000 characters; pass documents as parameters")
    depth = statements = tokens = 0
    for match in _LEXICAL_PARTS.finditer(text):
        token = match.group()
        if token.startswith(("--", "/*")):
            continue
        tokens += 1
        if token in ("(", "{", "["):
            depth += 1
        elif token in (")", "}", "]"):
            depth -= 1
        elif token == ";":
            statements += 1
        if depth > 64 or statements > 256 or tokens > 20000:
            raise NexoraQLParseError("Query complexity limit exceeded")


def parse(text: str, parameters: dict | None = None) -> list:
    """
    متن NexoraQL → لیست AST statements.

    Args:
        text: یک یا چند دستور NexoraQL (هر کدام با ; پایان می‌یابد)

    Returns:
        list[ast_nodes.*]

    Raises:
        NexoraQLParseError: در صورت خطای گرامری
    """
    if not text or not text.strip():
        return []

    _check_input(text)
    if parameters is not None:
        if not isinstance(parameters, dict):
            raise NexoraQLParseError("Parameters must be an object")
        _validate_values(parameters)
        if len(json.dumps(parameters, ensure_ascii=True, allow_nan=False)) > 16 * 1024 * 1024:
            raise NexoraQLParseError("Parameters exceed the 16 MiB request budget")

    parser = _get_parser()

    try:
        tree = parser.parse(text)
    except UnexpectedToken as e:
        expected = ", ".join(sorted(e.expected)[:8]) if e.expected else "?"
        raise NexoraQLParseError(
            f"Unexpected token {e.token.type} '{e.token}'. Expected one of: {expected}",
            line=e.line, column=e.column,
            context=e.get_context(text, span=40),
        ) from None
    except UnexpectedCharacters as e:
        raise NexoraQLParseError(
            f"Unexpected character at position {e.pos_in_stream}",
            line=e.line, column=e.column,
            context=e.get_context(text, span=40),
        ) from None
    except UnexpectedInput as e:
        raise NexoraQLParseError(
            f"Parse error: {e}",
            line=getattr(e, "line", None),
            column=getattr(e, "column", None),
        ) from None

    try:
        pending = [(tree, 0)]
        count = 0
        while pending:
            node, depth = pending.pop()
            count += 1
            if depth > 256 or count > 100000:
                raise NexoraQLParseError("Parse tree complexity limit exceeded")
            pending.extend((child, depth + 1) for child in getattr(node, "children", ()))
        result = NexoraQLTransformer(parameters).transform(tree)
        _validate_values(result)
        return result
    except VisitError as e:
        # خطای داخل transformer — orig_exc دلیل واقعی است
        raise NexoraQLParseError(
            f"Transform error: {e.orig_exc}") from None


def parse_one(text: str, parameters: dict | None = None):
    """parse یک دستور واحد — اولین statement را برمی‌گرداند."""
    stmts = parse(text if text.rstrip().endswith(";") else text + ";", parameters)
    if not stmts:
        raise NexoraQLParseError("Empty statement")
    if len(stmts) != 1:
        raise NexoraQLParseError("Expected exactly one statement")
    return stmts[0]
