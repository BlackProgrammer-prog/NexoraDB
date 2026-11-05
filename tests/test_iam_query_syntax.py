import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "NexoraDB/src"))
from nexoradb.parser import parse  # noqa: E402
from nexoradb.parser.errors import NexoraQLUnsupportedError  # noqa: E402
from nexoradb.parser.semantic import Executor  # noqa: E402


def test_presentation_role_examples_parse_and_have_no_embedded_credentials():
    statements = parse((ROOT / "examples/presentation/social_demo.nql").read_text())
    assert [type(statement).__name__ for statement in statements[:4]] == [
        "ShowSecurityUsers",
        "ShowSecurityRoles",
        "DefineRole",
        "DefineRole",
    ]
    assert statements[2].grants["users"] == ["read"]
    assert statements[3].parents == ["social_reader"]
    updates = parse(
        "ALTER USER $identity SET ROLES []; ALTER USER $identity SET GRANTS {};",
        {"identity": "immutable-id"},
    )
    assert updates[0].identity_id == "immutable-id" and updates[0].roles == []
    assert updates[1].grants == {}


def test_security_queries_never_fall_back_to_legacy_engine():
    with pytest.raises(NexoraQLUnsupportedError, match="native IAM"):
        Executor(object()).execute_text("SHOW SECURITY USERS;")
