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
        lexer="dynamic_complete",
        propagate_positions=True,
        maybe_placeholders=False,
    )


def parse(text: str) -> list:
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
        return NexoraQLTransformer().transform(tree)
    except VisitError as e:
        # خطای داخل transformer — orig_exc دلیل واقعی است
        raise NexoraQLParseError(
            f"Transform error: {e.orig_exc}") from None


def parse_one(text: str):
    """parse یک دستور واحد — اولین statement را برمی‌گرداند."""
    stmts = parse(text if text.rstrip().endswith(";") else text + ";")
    if not stmts:
        raise NexoraQLParseError("Empty statement")
    return stmts[0]